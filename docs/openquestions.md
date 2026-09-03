# Open Questions — Quake II PSX (`SLES-01534`) reverse engineering

## Where this stands

**The game plays from the front end to the credits.** One run walks all five units and eleven levels, takes
every unit boundary and its mission screen, and ends on `Extro FMV` — the level table's own name for the map
`QFMV` — playing `OUTRO1P.STX` (#121, #124). Every conformance command the harness carries passes over the
whole disc, and 29 of 29 tests pass.

**And the cinematics are wired the way the disc wires them (#124, #125, #126, #127).** The BOOT CHAIN opens
the game: two logo screens, four cross-faded full-screen images, and then `Intro FMV` — which #127 found is
asked for by QLOGOS writing game state 12, before the front end has ever been loaded, so `TAKE1BP.STX` plays
in front of the menu rather than behind it. A new game then opens on the front end's own reel,
`ROGUEINP.STX`, half a second after a difficulty is confirmed, which is what #126 read out of a store whose
writer sits in a delay slot. The third film is named by no movie-table record because its filename needs
thirteen bytes and the field is twelve. All three run
to the stop point their module passes the player — the outro is cut at frame **1,500 of 1,559** — and there
is now an ENCODER, which is the strictest check a format reading can be put to: 400 re-encoded frames decode
exactly, satisfy their own `bs_num_codes`, and rebuild the disc's own 7,712 sectors byte for byte.

**No questions remain open.** The four that stood here have all fallen: #6's fixup encoding and #28's three
clauses were solved in the code and never marked; #65 fell to noticing that the address it was hunting a
caller for is a LABEL rather than a function; and #8 fell to noticing that its "9-byte record" is two
arrays.

**The last two are the same mistake, and it is the one worth carrying out of this project.** Both stood on
a premise nobody tested — *this address is a function*, *this record is an array of structs* — and in both,
every search that followed was sound, exhaustive, and answering the wrong question. A negative result about
a wrong premise is indistinguishable from a discovery. What broke each was asking what would have to be
TRUE for the premise to hold, and testing that instead: an entry point has callers, and an adjacency graph
is symmetric. The second gave 100% against 25.6% on the first try.

| | how it fell |
|---|---|
| #6 | the low two bits of a fixup are a TYPE TAG, not alignment — which is why only 31% looked 4-aligned |
| #8 | the link record is two arrays, not nine interleaved bytes; the neighbour graph is 100% symmetric and every normal is a 1.3.12 unit |
| #28 | `+0x1C` is a per-level hook every map fills with the same `jr ra` stub; the playlist is walked by a cursor, so nothing loads `+0x22` |
| #65 | the mask is block A's third field — the one recorded as "always 0" — and it is zero on all 1,723 models, so the escape never fires |

**A lesson from #65 worth carrying to #8.** Three separate searches for a caller of `0x800B2410` came back
empty and were recorded as a finding twice. The address was never a function: it is a label inside
`0x800B1F90`, which is called normally. A negative result about a wrongly-typed premise looks exactly like a
discovery, and the only defence is to check the premise — here, "is this an entry point?" — before believing
the search.

A further four are **terminal**: they cannot be answered from this disc, and are marked `[!]` rather than
left looking like work outstanding. #30 needs an NTSC disc, #35 is unverifiable at N = 1, #36 is a field that
is zero in 100% of samples, and #38 is a standing security caution rather than a question at all.

---

Prioritised by how much each blocks the native port. **Tier 0** holds the items that have fallen, kept here
with their answers so nothing is silently dropped. Items in **Tier 1** stand between the project and a level
you can walk around in. The tail is cosmetic or archival.

**Where the blocking picture stands.** The world is now correctly textured. All four original Tier 1
blockers fell earlier — the texture codec, the model vertex-index base, the `Events` framing, and the world
scale down to one residue — and the binding that stood between decoded textures and *placed* ones has now
fallen with them: `MapMod.clut` (#1a), the tpage word (#1b) and the UV rotation (#11) were all read out of
the world renderer at `0x80068044` and then checked against the disc.

What made that possible is worth recording, because it changes what is attackable. The harness now carries
its own PS-X EXE loader and R3000A disassembler (`q2psx-inspect exe / disasm / xrefs / access / funcs`), so
questions about the original *code* no longer depend on an external disassembler session. Several items below
were parked on "needs a working disassembler session" and are now simply open work.

Animation followed, and is now complete. There are no per-part matrices to find (#2a): a part's transform is
a keyframed translation and quaternion in `CastList` block C. All 399 articulated models pose, and all 4,535
clips play, including the 3,241 variable-rate ones (#2c) — 2,036,080 keys decoded with none out of range.

Doors and lifts followed (#4a). The per-frame integrator turned out to be a seven-state machine that never
touches geometry — it accumulates a displacement in its runtime object and the zone draw adds it — which is
why an earlier pass, having proved every node in a zone shares one origin, wrongly concluded movers could not
displace anything.

Collision followed, and it turned out to be the case where inference from data had been most misleading. The
plane encoding was never the hard part; what mattered was everything the data could not show — that the
`extra[]` array is a **portal list**, that bit 15 of `firstPlane` marks a node solid, that every query runs in
16-bit wrapping arithmetic, that movement uses **`SecondaryCol`** rather than `PrimaryColl`, and that
`SecondaryCol` is `PrimaryColl` **eroded by the player's own 286-unit half-extent**, so the runtime moves a
point and never touches an entity's bounds. Three separate long-standing puzzles collapse into that last
fact. Details in #5.

Combat followed, and the reason it took three passes is worth recording. Two earlier passes swept the rodata
around the ammo arrays for an eleven-entry table of damage values and found nothing, so the port carried
figures inferred from the PC lineage and said so. The sweep was right and the conclusion was wrong in an
instructive way: **there is no table**. Each weapon has its own fire function, reached through a 12-entry
pointer array at `0x8009D704`, and damage, pellet count, spread, view kick and refire are immediate operands
inside it. Slot 0 of that array is `0x8004EB08` — `jr ra; nop` — which is the cleanest proof yet of the
1-based weapon indexing, because a do-nothing shot only makes sense as "no weapon".

Monster AI followed, and it is the largest single thing to fall so far. The whole engine-side framework —
target acquisition, the five movement verbs, the attack-state machine, eight-way chasing, stair stepping,
and the three-stage pursuit a creature runs when it loses you — is a transcription of PC Quake II 3.20's
monster code, and every constant lands on id's own number once the AI's scale is applied. Two things had
to be got right before any of it made sense, and neither is visible from the data: **the AI's world scale
is 12** where every other subsystem's is 10 (six unrelated constants agree on it), and **the vertical axis
is Y and points down**. Get either wrong and monsters cannot climb a step. `q2psx-inspect ai` checks 76
constants against the executable and all 76 agree. Details in #6 and FORMATS.md §14.

The creature modules came with it. There are only **seven** distinct ones on the disc, and each is decoded
by following its own code rather than searching its image — a route that turned up the entity's `pain` and
`die` hooks and the `skinnum` field along the way. All fifteen module instances decode with zero failures,
and the Soldier, which is most of the monsters in the game, is transcribed in full.

Everything downstream came with it: the damage function at `0x80057D54` and its twenty-one call sites, the
armour table at `0x8009C5EC` whose six values are PC Quake II's exactly, power armour, knockback and the
rocket jump's 3.2x self-multiplier, radius damage and its three read radii, the hitscan and rail paths, and
the projectile entity list at `0x800C91C0` whose per-frame sweep settles what a bolt's `+0x1A` and `+0x52`
mean. Full detail in FORMATS.md §13; `q2psx-inspect weapons` checks the data half against the disc, and
`tests/test_weapon.c` and `tests/test_combat.c` check the behaviour half.

~~Two negative results from that pass are load-bearing. The damage function **never kills** — there is no die
or pain callback, health simply goes negative and the entity's own think notices later — and for a creature
with an AI brain it does not even subtract health: it posts the amount to the module.~~ **BOTH WRONG, and
the mistake was one function deep.** `0x80057D54` is the OUTER damage function — armour, the mod table, the
twenty-one call sites — and it has exactly one interesting callee: at `0x800584B4` it calls **`0x800627F8`**,
which is id's `T_Damage` itself and which has that one caller and no other. Everything the negative results
said was missing is inside it:

    0x80062958   health -= damage       into (entity+0x24)+0x108
    0x800629B4   the floor, -9999
    0x80062A9C   the `die` callback at entity+0xA4
    0x80062AF4   the `pain` callback at entity+0xA0, with entity+0xA8 as its debounce
    0x80062AC0   M_ReactToDamage, which is the only writer of `oldenemy` in the image

So per-creature health, pain and death behaviour were never behind #6; they were behind reading one more
`jal`. The tail of that function is transcribed in `monster.c` as `q2_monster_damage_reaction` and
`q2_monster_death_use`, `M_ReactToDamage` in `ai.c`, and `q2psx-inspect ai` checks both against the disc.
One caution survives and is worth keeping: the call at `0x800584B4` passes `s2+0x2EC` as the target rather
than the entity its caller was handed, so which entity reaches T_Damage on which path is a separate
question from what T_Damage does with it.

**Player movement followed, and it is the pass that most changed what the port does rather than what it
knows.** §9.12 had the constants and the vertical integrator and said so; the player's own frame —
`0x8003A1C8`, 926 instructions, plus `0x80039AA4`, `0x8003E110`, `0x80019154` and the three velocity
integrators — had never been read. What came out is not the shape a Quake port is expected to have, and the
port had invented all four of the pieces that were missing.

- **There is no acceleration/friction pair.** One clamped-approach primitive at `0x8006FE3C` is the entire
  movement model, and the same function is the view-height ease, the turn-rate ease and the liquid buoyancy.
  Nothing in the player's call chain multiplies velocity by a coefficient. The port had `vel -= vel/4`.
- **There is no crouch button.** `INCROUCH` and `INLOWCROUCH` are `UserFuncs` primitives (`0x8002E5B4`,
  `0x8002F214`) that a trigger volume runs, so where you can crouch is authored per map. The port had a key
  bound to it. This also explains the `0x600` step-height halving and the 1600 speed cap, which had been
  sitting in §9.12 with nothing to set them.
- **There is no slope limit.** `entity+0x9C` gates the ground flag, the ground projection and the jump, and
  nothing in the image writes it on an entity — the five stores that looked like it belong to the **camera**
  array at `0x800D5C30`, whose `+0x108` holds 4000 where an entity's holds health. It stays zero, so any
  upward-facing surface is ground.
- **Velocity and position are one tick apart**, because the three collision moves consume `entity+0xEC` and
  the integrator that writes `entity+0xEC` sits at the end of the same function.
- **There are two movers, not one.** `0x80045ADC` runs the lift/slide/drop sequence only for an entity that
  is already grounded and not submerged, and a plain slide (`0x80045CA4`) otherwise. The port ran the
  sequence unconditionally, and because its drop is a whole step height against a jump's ~100 units of
  upward delta per tick, **a jump in a real hull produced no motion at all** — while every collision-free
  test still passed, because there is no step sequence without a hull. It is also the only way an entity
  ever *becomes* grounded, since the sequence requires it to be grounded already.

Two rules the port was missing entirely, and both are load-bearing. The **ground projection** at
`0x800459E8` replaces `vel.y` with `-(vel.x*n.x + vel.z*n.z)/n.y` at the top of every mover call for an
entity that was grounded — zero on flat ground, which is why gravity does not accumulate while you stand
still, and the surface-following component on a slope. And nothing clears velocity on contact, so it is that
projection on the *following* tick that produces the velocity change **fall damage** measures at
`0x80039CB4`. Fall damage therefore arrives a tick after the impact; a port that zeroes velocity on landing —
the obvious thing to write — makes every landing painless.

**The movement frame was followed a second time, and the second pass is mostly corrections to the first.**
The first pass read `0x8003A1C8` and got the model right; what it got wrong was three things it had stated
confidently, plus one whole output it had not noticed.

- **The control-style comparison was inverted.** `0x8003A670` is `slti style, 6` branching to the *eased*
  arm when the test fails, so the six mouse and stick styles set the look rate outright and the three
  `STANDARD` ones ease it. Backwards, the analogue styles coast after release and the digital ones snap.
- **Full digital deflection is 127, not 128** — eighteen sites, no exceptions. The wish target is
  `(maxspeed * axis) >> 7`, so the port was running 0.8% fast on every style.
- **The view angles are not the aim angles.** `0x80038260` composes three independently decaying kicks on
  top of them — firing over 30 ticks, damage over 150, landing over 90 — and `0x8004F40C` is where that
  becomes the camera. The port had the fall kick as a field nothing read, the weapon kick as a field nothing
  read, and a roll it computed and threw away. All three now reach the screen.
- **`entity+0x10C` bit 12 is the fly cheat, not a movement-basis switch.** It is the *first* test in the
  vertical integrator, and it also blocks the jump, the view recentre and the water-exit jump. The port
  steered by the view basis while still falling.

Two things were added rather than corrected. `0x80019154` — the pad, nine control styles, and the shared tail
that turns four configurable pad masks into eleven derived bits — had never been read, so the port invented
its own mapping and left `look_scheme` with nothing to set it. The load-bearing find in there is that **jump
and swim-up are the same button**: bit 22 is its press edge and bit 21 is it held. And the **view recentre**
at `0x8003A780` is a chord, not the `AUTOCENTRE` setting: hold both look buttons for a second tick and the
pitch walks itself level.

**One of §9.12's negative results is closed, and it closed the way the multiplayer search did — by being
pointed at the wrong binary.** `entity+0x98 & 0x20000`, the gate in front of the jump, is set by the
`DONTJUMP` `UserFuncs` primitive, whose body is one OR into that word exactly like `INCROUCH`'s. Nothing in
the *executable* sets it, which is what the original search found and reported correctly — the setter is in
the level data. **52 volumes across 15 maps** call it. Resolving a volume's record to its primitive also
gives the port the whole environment dispatcher: 37 `INCROUCH` volumes, 21 `UNDERWATER`, 13 `INWATER`, and
exactly **2** `INLOWCROUCH` — both on `SECURITY`, which means the 1600 speed cap and the halved step height
are reachable in two places in the entire game. `q2psx-inspect pmove` runs the census and
`tests/test_pad.c` checks the behaviour half.

One correction to this document's own §9.12: the liquid `0x08` arm's two branches were the wrong way round.
`0x80046050` branches on `vel.y < 1024` **into** the `-= dt*24` push, not into the ease. The net behaviour is
still a slow sink because the push loses to gravity, but implemented inverted it oscillates instead of
settling. Full detail in FORMATS.md §9.12; `tests/test_sim.c` checks 82 behaviours including both directions
of that convergence.

What is left is correctness and completeness elsewhere: area connectivity (#8) is undecoded, and the
relocatable module ABI (#6) still hides the level scripting and every creature's own attack figures.

**Lighting followed, and the interesting part is what turned out not to exist.** #9 asked how `SpaceLights`
is partitioned and #26 asked what a light's five `type` values do; both had been attacked from the data and
both had the same shape of answer, which is that the missing information was in a structure nobody was
looking at. `SpaceLights` is partitioned by the **secondary collision node**, through a halfword at +30 of
the very field #23 had recorded as having no reader — it does not; the reader is two bytes further in. And
the `type` byte is not a style at all: it is three bit-fields, and bits 3–5 select one of four **lens flare**
element lists in the executable, whose ghosts are positioned along the line from the screen centre to the
light. The long-running search for a flare texture was looking for something that does not exist: every
flare on the disc is untextured Gouraud polygons and lines.

Two more things fell out with them. The world is lit **entirely at build time** — the per-corner RGB tables
in `MapMod` are the PlayStation's lightmap and nothing at runtime modulates them — so `Lights` exists only
to shade *models* and to draw flares, at most three lights per entity because the GTE's light matrix has
three rows. And #2d's two caller matrices are now read: they differ by exactly the view rotation, because
the light directions stay in world space while positions go to the camera. Full detail in FORMATS.md §17;
`q2psx-inspect lights` and `lit` check the disc half and `tests/test_light.c` the behaviour half.

Multiplayer followed, and it is the clearest case yet of a search failing because it was pointed at the wrong
binary. Two passes swept the executable for deathmatch scoring and found nothing, correctly: the executable has
the **hook** and the maps have the **rules**. The whole multiplayer game is a 5,608-byte `LevelBin` module —
`QMULTI.C`, which names itself in a debug string — that is byte-identical on all thirteen arenas, and exactly
those thirteen maps are the ones whose `StartPos` carries `MultiSpawn` points. Six modes, three of them
selectable; the frag/time/round limits, the farthest-point spawn rule, the round machinery and the two exit
paths (`MPResults` or replay the round) are all read out of it. Full detail in FORMATS.md §16; `q2psx-inspect
multi` checks the data half against the disc and `tests/test_multiplayer.c` checks the behaviour half. Reading
it needed `levdisasm`, which is `moddisasm` pointed at `LevelBin` — so #6's ABI is now attackable from the
*level* side as well as the creature side.

Full structural detail, evidence and confidence markers live in [`FORMATS.md`](./FORMATS.md).

Legend: `[ ]` open · `[~]` partially resolved · `[x]` resolved (move the item, keep the answer)

---

## Tier 0 — Resolved (kept here with their answers)

- [x] **0. The multiplayer runtime. — SOLVED, and it was never in the executable.**
      The whole deathmatch game is `QMULTI.C`, a **5,608-byte `LevelBin` module byte-identical on all thirteen
      arenas** (MATRIX1…9, THEVAT, TIMS, PODCITY, FRAGTOWE) and present on no other map — a partition that
      matches, exactly, the set of maps whose `StartPos` names `MultiSpawn0`…`MultiSpawn7`. The executable
      contributes one call: `0x800396AC` ends with `(*(0x800B2F58))->[4](killer, victim)`, and `0x800B2F58` is
      the map's own module. Session state lives in the shared block at `0x800B2FE4` beside the GAME VARIABLES
      the menu already owned — time limit `+874` in **minutes**, frag limit `+876`, round limit `+878`, mode
      `+880`, players `+882`, `frags[4]` `+1204`, `team_frags[4]` `+1212`, `kills[4][4]` `+1220` — and
      **nothing in the executable writes any of it**, which is why an EXE-only search kept failing;
      `QFRONT`'s own `LevelBin` does.
      Six modes exist and their numbering is fixed twice over, by QMULTI's branches and by `QMRESULT`'s six
      scoreboard titles: 0 DEATHMATCH, 1 TEAM DEATHMATCH, 2 CTF, 3 TAG, 4 TEAM TAG, 5 VERSUS. **Three are
      unreachable**: the selector at `0x8010459C` writes only 0, 1 and 5; QMULTI's init zeroes the team/flag
      enable and then branches on it, so its flag setup is dead code; and no map on the disc names a flag
      spawn. Exit is one enum — a round that ended replays (engine state 19), a match that ended goes to
      `MPResults` (state 11), after a 450-unit banner.
      Two original behaviours are kept rather than corrected: the kill matrix is incremented on the
      **diagonal** (`victim*8 + victim*2`) and is read by nothing on the disc, and a team kill is charged
      **twice**, personally and to the team. Implemented in `src/game/multiplayer.[ch]`, checked by
      `q2psx-inspect multi` and `tests/test_multiplayer.c`, full detail in FORMATS.md §16.
      Still open, and deliberately not guessed at: the flag entity itself, so CTF/TAG/TEAM TAG scoring; the
      per-player 34-byte configuration record's two colour fields beyond "one is chosen when the mode is 1";
      and `multipics.lbm` / `multipic2.lbm`, which are still undecoded (#39).
- [x] **1. `SNDVRAM.DAT` section A pixel compression codec. — SOLVED.**
      Byte-oriented **PackBits with a destination-bounded loop**: `c < 0x80` copies `c+1` literals,
      `c >= 0x80` emits `257 - c` copies of the next byte, looping while output remains. The target is
      `width * height` bytes, and **texture-page records ignore their stored dimensions and are forced to
      128 × 256** (`0x80068B74` / `0x80068B7C`). `width` is bytes-per-row; the VRAM rect is `width>>1`
      halfwords wide. All **553/553** records on the disc decode to exactly the expected length — zero
      overrun, zero underrun — and no neighbouring codec or output-target variant comes close (best wrong
      alternative 152/553). `0x80` means 129 repeats, **not** the Apple PackBits no-op; it occurs 0 times in
      2,965,034 control bytes, so this is unfalsifiable from data but unambiguous in code.
      Implemented in `src/formats/vram.[ch]`. Full detail in FORMATS.md §4.1.
- [x] **2. `CastList` face vertex-index base. — RESOLVED.**
      `q2p_part`'s byte at `+2`, previously documented as `flags`, is the part's **base index into a shared
      per-model scratch window**, and should be called `vertBase`. Parts write their transformed vertices
      into the window at `vertBase`; faces index the window, not the vertex array. Verified over all 1,723
      models / 553,160 indices: **0** out of range, **0** causality violations, **100.0000 %** storage
      coverage on 1,723/1,723 models, and +0.7117 mean normal agreement / 98.13 % sign agreement on
      articulated intra-part faces against +0.0062 at `vertBase+1`. Reversing part order produces exactly
      18,995 reads of never-written slots, so forward file order is required.
      Also settled in the same pass: the vertex normal component order is **`z, x, y`**, not `x, y, z`
      (+0.7526 vs −0.0073 mean dot) — lighting was noise before this.
      Implemented in `src/formats/model.[ch]`. See #2a for what remains.
- [x] **3. World coordinate scale. — S = 10, INFERRED but tightly bounded.**
      The exporter's output lattice is generated by `10 · 2^k`. Enrichment `P(v ≡ 0 mod S) · S` over
      1,385,556 axis values is at background (≈1.0) for S = 6, 7, 9, 11, 13, 18 and 1.38 for S = 12 — fully
      explained as `P(mod 4)·P(mod 3)`. **The viable set is exactly `{5, 10, 20}`**, so `216 = 18·12` and
      `9000 = 800·11.25` are provably coincidences, which in turn means the physics constants were retuned
      and must be read from the PSX EXE rather than derived from PC values.
      Quantisation is CONFIRMED as `(int32_t)(true * S)` **truncated toward zero**, not `floor()` — this
      answers old sub-question 3a. Constants in `src/formats/worldscale.h`. See #3a for the residue.
- [x] **4. `Events` operand stream. — FRAMING SOLVED.**
      The record body is a **nested TLV item list**; the byte at `+2` is the **item count** (it is the
      interpreter's loop bound at `0x800279F8`) and the byte at `+3` is a **mutable flags byte** the runtime
      writes back (latch at `0x800279A8`, opcodes `0x14`/`0x15` clearing and setting bit 7). The old
      `sub`/`cls` reading is refuted. Exact on **4,179/4,179 records / 6,646 items** across all 164
      containers. Opcode dispatch (table `0x800ABD48`), the 43-entry `UserFuncs` binding table at
      `0x8009B6F0`, and one argument length per primitive (38 primitives, 3,760 items, zero exceptions) are
      all CONFIRMED.
      **Load-bearing caveat: the chunk is SELF-MODIFYING.** A load-time pre-pass at `0x80026DC0`, with its
      own table at `0x800ABCF8`, rewrites the `int16_t` slots inside items — on disc they are zone-local
      `Scene` node indices (up to 332), after load they are runtime object indices into a **48-entry** array.
      A port that feeds disc values to the exec handlers overruns that array by ~26 KB.
      Implemented in `src/formats/events.[ch]`. See #4a for what remains.
- [x] **1a. `MapMod.clut` → VRAM CLUT binding. — SOLVED.**
      The field is never read as a halfword. The world renderer at `0x80068044` takes the **high byte** as an
      index into the CLUT-id table (`0x80068288`, table pointer at `0x800B2EDC`) and the **low two bits** as
      the semi-transparency selector, choosing primitive code `0x3E` over `0x3C` (`0x800682A8`). The previous
      refutation tested the whole `uint16_t` against the id table — the wrong quantity. The other six bits of
      the low byte are build-time residue the engine never reads, set on **251,872 of 274,936** polygons,
      which is precisely why the halfword reading looked unsalvageable.
      Checked against the disc by `q2psx-inspect cluts`: the index is in range on **49 of 49** maps (max
      16…85 against CLUT counts of 36…259). The port's existing assumption turns out to have been right;
      it is now evidence rather than a guess.
- [x] **1c. `MapMod.clut >> 8 == 0`. — SOLVED. It is "do not draw", and reading it as a palette blacked
      out doorways all over the game.**
      1a settled that the high byte indexes the map's `clut4[]` array and left it there. The value **0** is
      not a member of that array's usable range: the used set is exactly `{0} ∪ [16, count_clut4_a)` on every
      map, real surface starts at 16 because the first sixteen entries are the reserved all-`0x8000` blocks
      (#1's correction), and `0x8000` is **opaque black**. So the 11,255 polygons that bind index 0 paint out
      whatever they stand in front of. An earlier pass looked at the same numbers and concluded "index 0 is
      real and must not be treated as no palette, it samples genuine texture content" — true of the *tile*,
      false of the *palette*, and the port drew black planes across doorways for it.
      What settles it is crossing those polygons with the draw order, which needed #7 to be readable first.
      Every one of the 11,255 lives in a node where **every** polygon binds index 0 — 1,749 such nodes, zero
      mixed — and across all 115 zones, 8,968 streams and 178,801 node references, **not one stream names one
      of those nodes**, against 98.0 % of every other node being named. They are the build tool's sealing
      planes: flat quads across openings, degenerate on one axis in 1,606 of 1,749 cases. Nothing else marks
      them — `Scene.flags08` is the ordinary `0x0000` on 1,739 of them.
      `q2psx-inspect surfaces` prints both halves of that comparison and fails if either ever changes.
      Acted on in `q2_mapmod_rec_is_sealing` (`src/formats/scene.[ch]`) and gated in `world.c` only on the
      index-order walk, since a supplied stream has already made the choice. FORMATS.md §3.1.4.
- [x] **1b. Where the GPU tpage word is assembled. — SOLVED. Texture pages are 4bpp, CONFIRMED.**
      Nowhere per-polygon: `0x80077FE8` precomputes twenty words at `0x800DDD3C` via
      `GetTPage(0, 0, tbl_X[i], tbl_Y[i])`, and that literal `0` is the colour-mode argument. `MapMod.tpage`
      indexes the table. The earlier search failed because it looked for the bit arithmetic inline; it lives
      inside libgpu's `GetTPage` at `0x8008A1C8`, once.
- [x] **11. `MapMod` `Poly.uvIdxFlags` bits 6–7. — SOLVED. A UV rotation, not render flags.**
      Vertex `j` takes `uv[(3 - f - j) & 3]`. Read from `0x80068118…0x800681D8`, then confirmed against the
      disc by a test the disassembly cannot have arranged: over the 31,931 quads that carry a rotation, the
      engine's rule holds texel-scale anisotropy to a mean of **1.33** against **7.61** for both rivals.
- [x] **12a. Does the world backface-cull? — YES, and the port was not doing it.**
      Never posed as a question, which is how it survived: the three quad linkers were described from
      `SETWIBBLE`'s point of view, as differing in *when they subdivide*, and "full culling" in that
      description was read as frustum work. It is an **NCLIP pair**, byte-identical in all three
      (`0x800AF8A8`/`0x800AF8C8`, `0x800AFB08`/`0x800AFB28`, `0x800AFD6C`/`0x800AFD8C`), so culling is not a
      per-variant policy at all — it is what linking a quad means:
      `SXY0,SXY1,SXY2 = v0,v1,v3; NCLIP; MAC0 > 0` draws, otherwise `SXY0 = v2; NCLIP; MAC0 >= 0` drops.
      `v1`–`v3` is a diagonal of the perimeter quad, so the two tests are its two halves and either one
      facing the camera is enough — which is what keeps a quad folded through the near plane from vanishing.
      Omitting it is not a performance question: sealing brushes' outward faces land in front of the rooms
      they enclose. In `world.c`, counted as `quads_rejected_back`.
- [x] **2a. `CastList` per-part transform matrices. — SOLVED, and there are no matrices.**
      A part's transform is a keyframed **translation and quaternion**, packed two words per part per frame
      in **block C**, which is the animation bank. The pose selector at `0x8006B924` walks the clip chain
      subtracting durations from the entity's tick counter until a clip contains the current time, then
      unpacks each part's key: three signed fields for the translation (bits 0–10 ×2, 11–20 ×4, 21–31 ×2)
      and three unsigned **half** angles for the rotation, which `0x800699E8` turns into a quaternion with
      the textbook Euler products against the `{sin, cos}` table at `0x800A5430`.
      Searching block C for matrix elements found nothing because none are stored — it holds packed angles.
      Verified by `q2psx-inspect anims` over the whole disc: **1,723 of 1,723** models walk, 4,535 clips,
      123,704 frames, **zero** keys escaping their block, all **399** articulated models animated, and the
      decoded quaternions are unit to **0.27 %** (|q|² 4087…4098 against 4096) — which no wrong angle scaling
      survives. The `12 + 8*numParts` size law that an earlier pass measured without explanation now falls
      out of the layout and holds on **1,265 of 1,265** single-frame clips.
      Implemented in `src/formats/model.[ch]` as `q2_model_anim_get` / `q2_model_pose_at`.
- [x] **2c. The variable-rate clip path. — IMPLEMENTED.** Clips with `flags & 1` — 3,241 of 4,535, so the
      common case — put their four-byte entries **per part** rather than per frame, each naming a stream of
      4-bit frame durations and that part's own key list, with keys interpolated in between (`0x8006B4DC`,
      and the quaternion slerp at `0x80069C64`). All 4,535 clips now decode: **2,036,080 keys**, none
      escaping its block. Three details the disc had to teach: a zero duration nibble is legal and advances
      the key index without consuming a frame; the tick clamp is to the *start* of the last frame, not the
      clip end; and the last key of a model's last clip has no successor inside block C, which the original
      reads anyway. The port's computed inverse cosine matches the original's 4096-entry table on 4,094
      entries and is one unit out on the other two.
      **The caller supplies sub-frame time for creatures and players.** The world-entity frame passes its
      residual 0…29 phase to `0x8007EB10`, whose `0x8007ECAC` arm stores `move.base + phase` at
      `entity+0x100`; the player driver at `0x8003DF58` independently adds each frame delta to that same
      halfword. `0x8006B5D8` divides it by ten and keeps the remainder for the interpolation above. Creature
      and multiplayer Male2 cursors now preserve that remainder instead of changing pose only on an AI/key
      boundary.
- [x] **39. The weapon stats table. — SOLVED, and there is no table.**
      Two passes swept the rodata around the ammo arrays for eleven damage values and found nothing. The
      sweep was right: every weapon has its **own fire function**, reached through a 12-entry pointer array
      at `0x8009D704`, and damage, pellets, spread, view kick and refire are immediates inside it. Slot 0 is
      `0x8004EB08` — `jr ra; nop` — a shot that does nothing, which only makes sense as "no weapon" and is
      the cleanest proof of the 1-based indexing.
      Read out with them: the damage function `0x80057D54` and its 21 means of death; the armour table at
      `0x8009C5EC`, whose six values are PC Quake II's exactly and whose rounding bias differs between
      single player (4095) and deathmatch (2048); power armour at `0x80057A9C`; knockback, including the
      3.2x self-multiplier that is the rocket jump and its −3072 single-player ceiling; radius damage at
      `0x80050810` with grenade 1000 / rocket 1300 / BFG 1300; the hitscan path `0x8004874C` and the rail
      `0x8004917C`; and the projectile entity list at `0x800C91C0`, whose per-frame sweep at `0x80047C6C`
      proves a bolt's `+0x1A` is a lifetime and `+0x52` its velocity.
      Two negatives worth keeping: the damage function **never kills** — no die or pain callback exists —
      and for a creature with an AI brain it does not even subtract health, it posts to the module.
      Implemented in `src/game/weapon.[ch]`, `combat.[ch]`, `projectile.[ch]` and
      `src/build/weapontables.[ch]`. `q2psx-inspect weapons` reads the data half back off the disc and
      compares it: **11 weapons, 3 armour classes, 22 sounds, 0 mismatches**. Full detail in FORMATS.md §13.
  - [x] 39a. **BFG flight and lifetime — SOLVED.** The spawner rotates the literal s16 vector
        `{0,0,768}` at `0x800AE9B4`; the rotation is a componentwise fixed multiply/`>>12`, not a
        Euclidean normalisation of the already-built view-matrix column. Its private think at `0x8004B2B4` advances each component as
        `vel * dt / 64` (`0x8004B90C..0x8004B9DC`), so the ball covers 12 world units per dt unit, or
        3600 per second. The 2400 at `0x8004BF20` is separately stored at entity `+0xF4` and counted down
        at `0x8004B8C0`, giving an eight-second lifetime; expiry takes the quiet entity-free path at
        `0x8004B8E4`. Implemented without the rocket-speed stand-in or a synthetic timeout explosion.
  - [x] 39b. **Grenade-launcher fuse — SOLVED.** The caller's 900 at `0x8004CF9C` is copied from stack
        argument 5 to entity `+0xF4` at `0x8004A2F8`; the think at `0x80049FE8` subtracts dt and detonates
        at zero. It is a three-second fuse. The fixed rotated `{0,2048,6144}` is the velocity and the shared
        mover advances it as `vel * dt / 320`, with its one-sided falling-Y clamp at 8192. The misleading
        380 is in Grenade3's held think at
        `0x8004A430`: it compares the owner's view-weapon position and gates a `6 * dt` charge of entity
        `+0x4C`. It was never written by the launcher spawner and is not a fuse.
  - [x] 39d. **Held hand-grenade state — SOLVED.** `Grenade3` is now its own three-state machine rather
        than an immediate Grenade2-style throw. The spawner at `0x8004AA6C` starts hidden in state 1,
        owner-attached, with raw forward charge **4096** and its **1650-tick fuse already running**. Its
        think at `0x8004A368` copies viewmodel `+0xA4..+0xAC`, plays `wep_hgrenc1b` when the model timeline
        crosses **261**, adds exactly **`6 * dt`** while position **380** is pinned, and crosses **411**
        into release. Release uses the read origin literal `{80,-50,200}`, rotates
        `{0,2048,charged}`, reveals the entity, plays `wep_hgrent1a`, and only then decrements grenade
        ammo (`0x8004A7C0`). If the fuse wins while held, it detonates at the hand and forces the view
        model to fire frame 2 / position 0 / 150 ticks remaining (`0x8004AA1C..0x8004AA40`) without
        charging ammo or playing the throw sound. Implemented across `weapon`, `projectile`,
        `viewweapon`, `simcombat` and the client; held state reuses the projectile record's collision-node
        sentinel, so save version 5 remains byte-compatible. `test_weapon`, `test_viewweapon` and
        `test_sim` pin attachment, charge, crossings, deferred ammo, release velocity, cook-off and
        owner damage. An identical `--headless --weapon 6 --shoot` frame-20 capture removes the old
        orange in-flight body/light while retaining the live hidden entity.
  - [x] 39c. **Residue: the chaingun's spin state. — SOLVED. It is three bands off the animation frame.**
        `+0x2C` is written by the FIRE-state driver at `0x800501C0`, and `0x80050180` is the table that
        decides what to write: **frame < 5 -> 1, 5..8 -> 2 while the trigger is held and 1 when it is not,
        frame >= 10 -> 3**, with frame 9 the one frame that writes nothing at all. `0x800501C8` then clamps
        the count to the rounds left, reading the ammo type off `0x8009DC5C` and the count out of the
        player's block. So a chaingun wound all the way up throws three rounds per animation frame and a
        tapped one throws a single round — the port defaulted to one for every band.
        Implemented in `src/game/viewweapon.c`; the clamp is the caller's, because each unit of
        `frame_fires` becomes one call of the weapon's fire function and a fire function that runs out
        reports dry. `tests/test_viewweapon.c::test_chaingun_bands` pins all eight cases.
        Measured on COMMAND, 400 frames, `--shoot`: the chaingun asks for **778 shots against the
        machinegun's 524** on the same clock, which is the bands doing their work.
- [x] **Viewport handling and drawing. — SOLVED, and one silent inversion came out of it.**
      The screen's constants were already read; what was not read was the *drawing*. Now transcribed from
      `0x800780C0`, `0x80076A74`, `0x80076764`, `0x80077230` and `0x80076E88`, and implemented in
      `src/screen/screen.[ch]`:
      the per-frame build's clear is a **three-way** decision — `0x800B2D94` suppresses every clear,
      `gp+18712` arms one full-screen env at OT[1], and neither leaves the per-viewport clears standing —
      with the loop bound in both armed arms being the **live** viewport count, so a three-player split's
      fourth quadrant is left alone;
      a viewport publishes its state in globals rather than passing it (the clip extent at `0x800B2C20`,
      already shrunk by the shake, the slice base at `0x800B2D60`, the index at `0x800B2C1C`);
      **`view+144` bit 0 gates the world draw** and is set with bits 1-2 when a player's camera is installed;
      **`view+264 / 4` is an entity cut-off**, compared against a distance inside the model draw at
      `0x800698E0`, so entities stop at a quarter of the world's distance;
      the **damage flash** belongs to the viewport, not the overlay — a full-viewport semi-transparent `TILE`
      at the slice's frontmost bucket, with **four** colour modes and a countdown that runs once per drawn
      frame — and the port's copy has moved out of `src/game/hud.c` accordingly;
      the **overlay camera** publishes itself as viewport 0, never shakes, and does not recompute the entity
      cut-off; and the **performance meter** is nine two-pixel bars at OT[52], six of whose accumulators reset
      as they are drawn.
      **The inversion.** `ClearOTag` at `0x800837C0` builds a *forward* chain and `DrawOTag` is handed entry
      0, so a higher bucket draws later and is therefore nearer — which the background env at OT[1], the
      per-viewport env at slice bucket 1 and the flash at slice bucket 50 all independently confirm. The port
      was linking primitives by depth into *ascending* buckets and then walking forward, i.e. drawing far
      geometry over near. `psx_ot_add` now takes a depth and inverts it; `psx_ot_add_bucket` names a bucket.
      Also read out with them, and previously carried as guesses: the five-halfword table at `0x800B36D8`
      (`{32, 0, 32, 64, 96}`) is the **semi-transparency field of a tpage word**, indexed by a face's texture
      byte shifted right by five, so an unset selector is B+F rather than the half blend; and `InitGeom`
      leaves **ZSF3 = 341, ZSF4 = 256**, which no code overrides.
  - [ ] Residue: **what `view+308` points at.** Every layout stores its own argument there as a word and the
        callers pass function addresses (`0x800337D0` for one player, `0x80033D30` / `0x80034288` for the two
        splits). It is a per-view callback; what invokes it was not followed. The port expresses it as the
        `view` hook in `q2_screen_hooks`.
  - [x] Residue: **`view+156` is not a depth scale** — **ANSWERED, and it is angular.**
        The reader is `0x80038374`, inside `0x80038260`, which is the **view-angle composer**: `view+154`,
        `+156`/`+158` and `+160`…`+164` are three view KICKS, each with its own deadline and decay period —
        90 ticks for the landing kick, 150 for the damage kick, 30 for the firing kick — and each is scaled
        by `((deadline − now) << 12) / period` and added to the pitch, yaw and roll before `0x8004F40C`
        turns the result into the camera. So the "running three-halfword total" is an angle triple, not a
        wobble, and the previous reading had the *arithmetic* right and the *meaning* unlocated. The port
        applies all three now: FORMATS.md §9.12.11a, `q2_sim_view_angles`, `tests/test_pad.c`.
  - [x] Residue: **`0x800D8D78` is the screen-area table** (region-relative; `0x800D8FE8` in NTSC).
        `0x800689F4(p, 1)` stamps area 1's per-viewport record with the tick at `0x800B2DE4`. The 64-byte
        area stride and 16-byte viewport stride lead to the screen record and the two final batch lists
        reconstructed in 7b; the duplicate call is idempotent.

- [~] **2d. What the two caller matrices hold when a model is drawn. — ANSWERED; one residue left.**
      The caller is the entity draw at `0x8006B924`, and it fills a draw context whose fields `0x800B1F90`
      then consumes at fixed offsets:

      | context | holds | becomes |
      |---|---|---|
      | `+0x60` | the entity's own 3×3 at entity `+0x2C0`, copied halfword by halfword | the **light** matrix, after the part's quaternion is composed into it (`0x800B21B8`…`0x800B223C`) |
      | `+0x74` | three light DIRECTIONS the gather at `0x8006AFE8` leaves there | the rows of that light matrix |
      | `+0x8C` | `view × entity`, built by `MulMatrix(0x800B28B8, entity+0x2C0)` at `0x8006BB94` | the **rotation** matrix, again composed with the part's (`0x800B2240`…) |

      So the two differ by exactly the **view rotation**, and that asymmetry is correct rather than an
      oversight: the light directions are in world space, so rotating them into the camera as well would
      make a model's shading swing as the player turned. `0x8006B298` does **not** scale that entity
      matrix: its destination is the separate matrix at `0x800DDD1C`, installed by `SetLightMatrix` at
      `0x8006BBD4`. It scales the gathered light directions by
      `(entity+0xFC · entity+0xFE) >> 11`; the geometric rotation installed immediately afterwards comes
      from entity `+0x2C0` and never reads either halfword.
      Implemented: `src/game/modeldraw.c` composes both the same way, and shades through `NCS`.
      **Residue:** posing an articulated model flat still does not restore agreement between its extents and
      the header's `ext2`/`ext3` (4/399 and 15/399 against 0/399 and 5/399 unposed), while static models are
      unaffected. Now that the matrices are known to carry no per-model scale at all, the
      remaining explanation is that `ext2`/`ext3` are authored bounds rather than measured ones.

- [x] **HUD. — SOLVED, and the answer is that there is no status bar.**
      The game shows no health, ammo or armour readout. That was established by enumeration, not by looking:
      every format string the executable hands to its text layer was resolved back through its `lui`/`addiu`
      pair — all 30 call sites of `0x80043518`, all 3 of `0x800434B8`, and every `sprintf` in the image — and
      none of them formats a player statistic. The only two readers of the font table at `0x8009D554` are the
      markup interpreter and the menu's glyph loop, so no separate digit drawer exists either.
      What the overlay *is*: a markup language (`0x80042328`) drawing 8x8 sprites out of `chars.lbm`, a
      four-slot notification ring retiring one line every 60 ticks, a centred line with a backdrop that fades
      by shrinking, a crosshair, and a damage flash whose colour says whether armour or flesh took the hit.
      The atlas lands at VRAM (0, 384) — slot 15 with a v origin of 128, read out of `0x8003FEA4` →
      `0x8006901C` → `0x800691A8` — and its palettes are in the executable's own bank at `0x800A2FEC`, not on
      the disc. Full detail in FORMATS.md §11; implemented in `src/build/hudtables.[ch]` and
      `src/game/hud.[ch]`, checked by `q2psx-inspect hud` and `tests/test_hud.c`.
      This also settles the menu's open item: the menu font is the same atlas plus `frontend.lbm`.

---

## Tier 1 — Blocking: cannot render or load a level

The residues of the resolved blockers keep their parents' numbers.

- [~] **2b. Cross-part index resolution for 21,217 faces (15.3 %), all inside the articulated
      models.** Last-writer-wins and a rival arithmetic rule both resolve 100 % of indices in range and
      differ on 18,283 articulated faces; geometry alone cannot separate them. Last-writer-wins wins on
      coverage (100.0000 % / 1,723 models vs 99.6131 % / 1,462) and is what the module implements, behind one
      function so it can be flipped. **It is now decidable**: the two rules transform those borrowed vertices
      with different parts' poses, and the poses are decoded, so posing a model both ways and comparing the
      seam quads against the stored vertex normals settles it.
- [~] **3a. The PSX-texel to PC-texel ratio — the last step to CONFIRMED for S = 10.**
      The texture measurement fixes 10 world units per *PSX* texel, which is one equation in two unknowns.
      S = 10 requires PSX 64×64 tiles to be 1:1 with PC Quake II 64×64 textures at scale 1.0; a 2:1
      downsample of 128×128 sources — standard PSX practice — gives S = 5. Nothing on this disc distinguishes
      them. Two supporting arguments were refuted: the 1280 secondary texture span is a per-surface artistic
      choice (**729 of 2,111 texture keys, 34.5 %, appear at two or more canonical world spans inside one
      map**), and the 286-unit cube "agreeing with Quake II's player to 2 %" is numerology (the eye sits 576
      above the feet on a 572-tall body, ratio 1.007, against PC's 46/56 = 0.82).
      *Attack:* now cheap, because #1 is solved — decompress one texture payload, count its pixels, compare
      against the PC Quake II `.wal` it derives from.
  - [ ] 3b. Residue: `|v| mod 10 == 1` (8.89 %) and `== 8` (10.43 %) are 3–4× the off-lattice background,
        and a single truncation can never *increase* a magnitude. Either the exporter dithered ±1 on top of
        the truncation, or more vertices are genuinely off-lattice than the residue-2…7 background implies.
        Does not affect the decode rule; it means the grid-snapping model is incomplete.
- [x] **4a. `Events` `fnB` motion integrators and the 92-byte runtime object. — SOLVED.**
      The per-frame handler every mover installs is `0x80025658`, reached from the sweep at `0x8002DC04`
      that walks 48 objects of 92 bytes at `0x800D6BB0`. It is a seven-state machine — at rest, opening,
      arrived, closing, blocked, delay, held open — driving `pos += speed * dt` along one axis, clamped to a
      signed target, with the delta applied down the `+0x30` chain, an obstruction veto at `0x80051EC0`, a
      16-tick blocked retry, and positional sounds at the node's bounding-box centre.
      **It never touches geometry.** The displacement accumulates in the object at `+0x12`, and the zone
      draw at `0x800678EC` adds it to the node's camera-space position as it draws. That is the piece the
      earlier pass was missing when it concluded from the shared node origin that movers cannot displace
      geometry: they displace it at draw time. Now wired into `q2_world_build_ot`, and visible —
      `bmodel <map> <zone> <group> out.ppm 1` opens a BASE1 lift and it moves 1,151 units up the Y axis.
      The full field map is in FORMATS.md §2.9.1. It corroborates the port's mover model, which was derived
      independently from the on-disc payloads: same states, same single axis, same absolute-valued speed.
  - [ ] 4b. Which physical chunk backs `gp+0x174` versus `gp+0x178`. Both are set from loaded chunk pointers
        (`0x8007AD54`, `0x8007C234`) and the pre-pass runs immediately after `gp+0x178` is set
        (`0x8007C278`). Given `COMMON.Events == ZONE0.Events` in 49/49 and only the `Scene`-index slots
        differing per zone, the natural reading is map-wide copy vs zone copy — but it was not established.
  - [ ] 4c. The `STRING` operand string namespace (165/363 resolve against the map's own `Strings`) and
        `MISEVENT` (**0/93** — entirely unlocated, not partially located). And record `flags` bits 3–5: the
        loader default `if ((f & 0x28) == 0) f |= 0x10` confirms `0x08`/`0x10`/`0x20` are alternatives with
        `0x10` as default, but the three categories are unidentified.
- [x] **5. Collision plane point encoding. — SOLVED, and the whole collision model with it.**
      The encoding was never the interesting part. It is an unsigned halfword offset from the owning node's
      `bboxMin`, read that way at four separate sites (`0x800441DC`, `0x800446C0`, `0x80044384`,
      `0x80043FB0`), and the "95.6 % / 99.85 % confirmed" figures were measuring how well a guessed reading
      matched a geometric expectation rather than reading the code.
      What the code says, in full, is in FORMATS.md §3.4. The load-bearing parts:
      **`firstPlane` bit 15 is a SOLID flag** (`bltz` at `0x80044190` and `0x800442BC`, every reader then
      masks `0x7FFF`); **`extra[]` is the PORTAL LIST**, `(planeInThisNode << 11) | neighbourNode` plus the
      matching plane index in the neighbour; **`d`'s low byte is the node's contents id**; and every query
      runs in **16-bit wrapping arithmetic** relative to the node's minimum corner, which a 32-bit port
      silently diverges from on a long move.
      **The movement hull is `SecondaryCol`, not `PrimaryColl`** — the mover at `0x80045144` loads the
      context the loader filled from it — and `SecondaryCol` is `PrimaryColl` **eroded by 286 on every
      axis**, i.e. the configuration-space hull of the player's own cube. That is measured, not asserted:
      over 5,275 axis probes across all 115 zones the free space differs by exactly 286 in 37 % and by
      286 ± 2 in 52 %. It explains the three things that never fitted — why Secondary has *fewer* nodes on 9
      of 115 zones, why the player path contains no bounds access, and whether 286 is the real hull (it is).
      Convexity is never assumed by the engine, so the 148 non-convex nodes are harmless.
      `q2psx-inspect coll` checks every invariant the transcribed code depends on across all 230 chunks:
      **0** out-of-range link nodes, planes or back-planes of 94,642 links; **0** nodes over the 32-plane
      limit and **0** hulls over the 2048-node limit that the 5/11-bit packing imposes — both holding exactly
      at their boundary; 94,620 reciprocal portal pairs; **0** traces leaving the hull. `q2psx-inspect walk`
      then drops a player into **47 of 47** maps: every spawn lands in a cell, every player grounds, none
      ever leaves the hull.
      Implemented in `src/formats/collision.[ch]` and `src/game/trace.[ch]`.
- [x] **6. `LevelBin` / `CreAIBin` module ABI and the `Rel` fixup encoding. — FULLY FALLEN; the second half
      was solved in `reloc.h` and this entry never caught up.**

      **The 31% was the clue, not the obstacle.** "Every fixup is a valid in-`Bin` offset, but only 31% are
      4-aligned, so it is not a plain word-address list" is exactly right and the conclusion drawn from it
      was backwards. The low two bits were never alignment — they are a TYPE TAG:

          offset = entry & ~3     type = entry & 3

          0  WORD32    *t += base
          1  HI16      consumes ONE EXTRA raw word as an addend, then
                       *t = (*t & 0xFFFF0000) | (((addend + base + 0x8000) >> 16) & 0xFFFF)
          2  LO16      *t = (*t & 0xFFFF0000) | ((*t + base) & 0xFFFF)
          3  TARGET26  the jump target, shifted, rebased and re-packed

      The residue histogram decomposes exactly into the four type counts, which is the check.

      **The HI16 addend word is the trap and is worth carrying.** It is a raw value rather than a tagged
      entry, and a decoder that fails to consume it stays in step almost all the time — a flat scan scores
      **99.78%** against 100% for the correct parse, because only the 216 addend words whose low bits happen
      to be `0b10` are misread as LO16 entries. Close enough to look like success, wrong enough to corrupt a
      module. HI16 needs the separate addend because the modules carry no high half at all: every one of the
      **28,191** HI16 sites is a `lui` whose immediate is zero.

      Implemented in `src/formats/reloc.[ch]`, and the disc is the check: **15 of 15 creature modules over
      13 maps relocate with zero failures**, the level scripts run, and QFRONT's and QENDMIS's modules give
      up their menu pages and their movie table. What is no longer open is the **ABI**,
      and with it the whole engine side of monster AI.

      **The interface record is read.** `0x8007D990` builds a **304-byte, version 1** import table at the
      module's base and fills sixty function pointers into it. That table is the complete list of services a
      creature module can reach, and it names them: the five shared AI verbs at +0x54…+0x64, the vector
      helpers at +0xB8…+0xCC, `visible` at +0xE4, the melee attack `fire_hit` at +0xEC, the damage function
      at +0xF0, and — the one that unlocks the rest — a **class-method setter** at +0x118. Full table in
      FORMATS.md §14.11.

      **Both dispatch mechanisms are read.** A module writes its `stand`/`walk`/`run`/`dodge`/`attack`/
      `melee`/`sight` callbacks *directly into the entity* from its spawn function, and separately registers
      per-animation-frame handlers into a 256 × 32 class method table at `0x800D519C`. The Soldier's spawn
      (`BASE1` module+0x1604) writes exactly id's seven fields at exactly the offsets the engine reads, with
      `melee` explicitly zeroed, plus `mass = 100` — which is id's soldier, value for value, from a decode
      that was not tuned to produce it.

      **So the engine half of the AI is done and checked.** `ai_stand`, `ai_walk`, `ai_run`, `ai_charge`,
      `ai_move`, `ai_run_slide/missile/melee`, `ai_checkattack`, `FindTarget`, `FoundTarget`, `HuntTarget`,
      `visible`, `range`, `infront`, `M_MoveFrame`, `M_ChangeYaw`, `M_walkmove`, `M_MoveToGoal`,
      `SV_movestep`, `SV_StepDirection`, `SV_NewChaseDir`, `SV_CloseEnough` and the player trail are all
      transcribed in `src/game/ai.[ch]`, `aimove.[ch]`, `aiworld.[ch]` and `monster.[ch]`. `q2psx-inspect ai`
      reads 76 constants back out of the executable and compares them against the port: **76 of 76 agree.**
      The AI's world scale turns out to be **12**, not the 10 established elsewhere — six independent
      constants agree on it — and the vertical axis is **Y, pointing down**. FORMATS.md §14.

      **The modules themselves are now decoded too.** There are only **seven distinct creatures** on the
      whole disc — Soldier, Tankcomm, Gunner, Insane, Arachner, Infantry, Berserk — over fifteen module
      instances on thirteen maps. `src/game/creature.[ch]` follows each module's own code to its class
      bytes, animation speed scale, mass, callbacks, moves and frames; `q2psx-inspect creatures` runs it
      over the disc and **15 of 15 decode with zero failures**. Two entity fields fell out of it that
      nothing else had located: `+0xA0` and `+0xA4` are the entity's own **pain** and **die** hooks, and
      `+0x3A` is **skinnum**, which is what selects a Soldier's weapon.
      The **Soldier** — seven of the thirteen AI maps, so most of the monsters in the game — is fully
      transcribed in `src/game/cre_soldier.c`: all fourteen think indices, all eight callbacks, all
      eighteen moves. Its constants are id's (`random() < 0.8` as 26214/32767, and so on).

      **AND ALL SEVEN ARE NOW TRANSCRIBED.** This entry used to open the next paragraph with "reading 37
      MIPS functions one at a time was not going to happen"; it happened. `cre_tankcomm.c`,
      `cre_gunner.c`, `cre_infantry.c`, `cre_arachner.c`, `cre_berserk.c` and the new `cre_insane.c` join
      `cre_soldier.c`, and `q2psx-inspect creatures` reports **every callback of every module written by
      hand**. The paragraph below is kept because the decoded-action path is still what a creature this
      port does not know would run on, and because the decoder is the CHECK on the transcriptions: where
      the two disagree, one of them is wrong, and on this pass it was twice the decoder — a `rand() % 6`
      reported as `% 3` because the multiply-back reconstruction stopped one shift short of the doubling,
      and a think walked past its own `jr ra` into the next function because that function was an export
      and therefore not in the decoder's entry set.

      **The other six creatures' think functions are decoded rather than transcribed.** Reading 37 MIPS
      functions one at a time was not going to happen, and writing them from the Soldier's pattern would
      have been invention. So the same trick that recovered the move tables was pushed one level down: a
      think function's *actions* are followed — the sound it plays, the claw it swings with its own aim,
      damage and kick, the frame it jumps to, the aiflags it sets. **51 of 51 think indices across the disc
      decode to an action, and all 7 creatures act.** The decode is validated against the Soldier, whose
      functions were read by hand first and which it reproduces independently; and the numbers it recovers
      are id's — the Berserk's two attacks come out at damage 15 and 5 with kick 400, which is
      `berserk_attack_spike` and `berserk_attack_club` exactly.
      What remains approximate is branch conditions: a step behind a branch is marked and handled by two
      stated rules rather than modelled. FORMATS.md §14.14.

      ~~Also still open is where a specific creature's health subtraction, pain threshold and per-attack
      damage live.~~ **CLOSED, and the three lived in three different places.**

      * **The health subtraction is in the executable**, at `0x80062958`, inside `0x800627F8` — which is
        not something the damage function "posts to" but `T_Damage` itself, reached from `0x80057D54` at
        `0x800584B4` and from nowhere else. See §14.15.
      * **The pain threshold is per creature and is an ARGUMENT**, not a table: `T_Damage` hands the
        amount to the module's `pain` at entity+0xA0, and four of the seven branch on it — the Tank
        Commander at `damage <= 10` / `<= 30` / `<= 60`, the Gunner at `<= 10` / `<= 25`, the Berserk at
        `< 20`, and the Berserk's `die` at `>= 50` for the long death. This port's callback carried no
        damage argument at all, so every one of those took one arm forever.
      * **The attack figures are immediates in each module**, passed to the contiguous `monster_fire_*`
        family at import `+0x80`…`+0x9C`. All of them read, and all of them id's:

            Tankcomm  blaster 16/800   rocket 50/550   bullet 20, kick 4, spread 300/500
            Gunner    grenade 50/600                   bullet  3, kick 4, spread 300/500
            Infantry                                   bullet  3, kick 4, spread 300/500
            Arachner  railgun 50, kick 100
            Soldier   blaster  5/600   shotgun 2, kick 1, spread 1000/500, 12 pellets

        `DEFAULT_BULLET_HSPREAD` 300 and `VSPREAD` 500 recurring across four unrelated modules is what
        says the argument positions are right rather than merely consistent.

      Combat against creatures was "complete on the player's side and hollow on theirs" and now is not:
      six of the seven could hunt the player down and never hurt them, because the port's fire hook took
      a single `int` and the client declined anything that was not one of the Soldier's three flash
      tables. It carries the figures now. What IS known from the executable, and is implemented, is which mod each
      creature attack carries: a contact hit is mod 7 (`0x800612F0`), a thrown grenade is the launcher's own
      spawner through the wrapper's 600-valued ballistic solve; that same value is passed as its `+0xF4`
      timer rather than the player's 900 (`0x80061724`). A creature rocket is `0x8004AF28` with the aim
      scaled by 3/2 (`0x80062164`).
      *Attack:* the modules disassemble now (`q2psx-inspect moddisasm`), each carries its animation names as
      plain strings, and its spawn function is export 0 — so the remaining work is per-creature transcription
      rather than another format problem.

      **A SECOND interface block exists, and it is not the same one.** Following the item spawner turned up
      `0x80079818`, which builds a **1268-byte block at the fixed address `0x800B2FE4`** — the size is its own
      first word — and fills **240 slots** with executable addresses and shared-global pointers. That is
      distinct from the 304-byte per-module table `0x8007D990` writes at a module's base: this one is global,
      lives in the engine's own data, and mixes services with state (the GAME VARIABLES settings the pause
      menu writes sit at `+886`…`+904` of it). It is what makes the old finding under #10 make sense — the
      Population globals have no reader *in the executable* because the readers are in the modules, and this
      is the door they come through. Slots 9, 11, 13 and 70 are the Population/item side; the full list is in
      FORMATS.md §15.5. What is still unread is which slot numbers the *modules* use for what, which is the
      same per-module transcription problem as the paragraph above.

---

## Tier 2 — Blocking: degrades the level badly, does not prevent loading

- [x] **10. `Population` `spawn.classId` target table. — SOLVED.**
      It is not an index into anything on the disc. The id is stored beside a **name** in a 48-byte-stride
      table at `0x800A3368`, and the engine reaches it from the other side: a `CreAI` module's 16-byte
      preamble starts with a 12-byte name, and the loader at `0x8007D990` looks that name up through
      `0x80057A18`. Each record also carries the class's `health` and a size-scaled offset.
      The health column is what proves it: the values are PC Quake II's own, creature for creature — three
      `Soldier` records at 30, 20 and 40 (shotgun, light and machinegun guards), `Infantry` 100, `Flyer` 50,
      `Gladiator` 400, `Jorg` 3000 — and nothing in the decode was tuned to produce them.
      `q2psx-inspect classes` checks it disc-wide: **651 of 651** spawn records resolve to a class, and
      **651 of 651** of those classes name a model the same map ships. Implemented in
      `src/build/classtable.[ch]`. Full table in FORMATS.md §9.8.
- [x] **10b. One model part textures wrong. — ANSWERED, and nothing textures wrong.**
      The Soldier is correct. Every clause of this entry except the existence of violet pixels was wrong,
      and the way each one fell is worth more than the entry was.

      **The camera was behind the model.** `q2psx-inspect model` defaults to yaw 0, which is the Soldier's
      BACK, and three passes described "the head" without ever turning it round. From the front the model
      is a Quake II soldier in every detail — grey helmet, visor, green torso, bare arms. The violet is on
      the back of the collar and is not visible from any angle a player meets one at.

      **It is not noise and it is not a wrong palette.** The violet is palette entries 11, 13, 14 and 15
      of CLUT 107, and those four are a clean four-step ramp — (104,72,192), (80,40,160), (56,24,104),
      (40,16,64). Entries 0-10 of 107 are byte-identical to 113's, so what varies between the model's
      palettes is only an ACCENT of four: violet in 107, red in 113, brown in 114. Over the patch behind
      the head, 33 of 255 texels take the accent and they draw a coherent ribbed structure, not speckle.
      A four-step ramp used by a coherent pattern is authored.

      **And the binding reproduces three separate atlases exactly.** The same model is re-laid-out per
      map: part 1 is pages 1-5 / textures 39-45 on BASE1, pages 1-7 / 46-60 on BASE2, and pages 3-7 /
      76-88 on WASTE1. Rendered through the port's page-and-CLUT rule the three come out **pixel
      identical** — 22 and 86 differing pixels out of 245,760, none of them in the violet. A binding
      defect cannot make three independently authored palette tables agree to 0.03%.

      So the answer is that **the disc says violet**, and the port is drawing what it is given. What
      remains open is only the narrow possibility that something rewrites a CLUT at runtime for a
      particular entity — the `skinnum` field exists and the class table gives the Soldier three variants
      — which would be a different mechanism entirely and is not evidenced by anything found here.

      The rest of this entry is kept because the four negative results are load-bearing for anyone who
      later suspects the model texture path.

      **It is not part 2, and part 2 is not broken.** Drawing one part at a time (`Q2_ONLY_PART` while
      bringing this up) shows part 2 IS the head — a helmet, correctly textured in olive and grey — and
      the violet belongs to **part 1**, the torso, whose faces reach up over the neck and paint on top of
      it. The old attribution came of noticing that part 2 is the only part using page 6 and texture 46 and
      assuming the odd part out was the broken one.

      **The suggested attack is answered and it exonerates the binding.** Page 6 under CLUT 114 — part 2's
      pair — decodes to the helmet, visor and all. There was never a wrong palette to find there.

      **The violet is CLUT 107**, which the pixel is traced to rather than reasoned to: probing the frame
      buffer at a violet pixel reports `tpage 0x0032 clut 0x4683`, which is page 1 and clut4 entry 107,
      from part 1's `texture` 39 against `clut4_count_a` 68. And 107 is not a rogue palette: its entries
      0-10 are the same olive ramp as 113's, and only 11, 13, 14 and 15 differ — violet where 113 has dark
      red. So the texels the head samples land in the palette's TAIL, and the tail is what varies.

      **Four candidate causes are eliminated against the executable, not against intuition:**

      - The CLUT base is `clut4_count_a`. `0x8006A3FC` adds the halfword at `0x800B2EEC`, and the only
        write to it is `0x80068AD0`, which stores the section header's byte `+2` — `clut4_count_a` — as
        `img_open` parses it. Confirmed, not assumed.
      - The CLUT id is `q2_vram_clut_word`. The id table is built at `0x80076378` as
        `((y & 0x1FF) << 6) | ((x & 0x3FF) >> 4)` over `x = 16*(i&3)`, `y = 256 + (i>>2)` — the port's
        formula character for character.
      - The face record is right. `0x8006A390` sets the cursor to `faces + 12` and steps 16, reading
        `flags` at `+0` and `texture` at `+1` and the four UV halfwords at `-8, -6, -4, -2`: exactly
        `v[4]` at `+0`, `uv[4][2]` at `+4`, `flags` at `+12`, `texture` at `+13`, stride 16.
      - The page is right. `tpageTable` is twenty `GetTPage(0, 0, slotX[i], slotY[i])` at `0x8007801C`
        over the same two slot tables the image loader uses, and face page 6 lands on VRAM cell 7.

      **And one thing the reading gained on the way**, which is checked in and not residue: the emitter
      stores file `uv3` into POLY `uv2` and file `uv2` into POLY `uv3` (`0x8006A3E4` / `0x8006A3F0`), and
      the linker does the same with the corners (`0x800B2478` puts file `v3` in `SXY2`). That is the
      file-perimeter to hardware-Z conversion, and it confirms that a model quad's corners are in
      PERIMETER order in the file — so this port's fan split is right for models exactly as it is for the
      world.


- [x] **7. `SortData` encoding. — SOLVED, and the world is not depth-sorted at all.**
      "No fixed per-node record" was the finding, not the obstacle: there are no records. It is a
      **self-describing variable-width opcode stream**, and the reader is the zone draw's own, inlined seven
      times between `0x80066B70` and `0x800676D8`. Bits are LSB-first inside little-endian 32-bit words
      against a mask table at `0x8009FBF0`; the start offset is a **byte** offset from the viewport record's
      `+28`, word-aligned down with the remainder consumed as bits.
      The header is seven fields, each holding its width **minus one** (widths 1…16), in the literal order of
      the `slti` bounds: `4,3,4,3,3,3,4` bits giving `w_base, w_op_short, w_op_long, w_f1, w_f3, w_f4, w_f2`,
      then `w_base` bits of `base`. Then opcodes: **0** ends, **1** is a screen-region change (historically
      called an entity record; `f1` names a Scene/Points marker, `f3` its parent region, `f4` its draw area,
      and `f2` is branch data), **2** switches between a windowed mode that
      adds `base` and an absolute mode that does not,
      carrying the replacement opcode at the new mode's width, and **≥3** is a scene node, `op - 3`.
      **The load-bearing consequence.** The two apparent starts are consecutive operations, not alternatives:
      `0x80066970` seeds the camera area's full-view screen record at bucket **45**, then `0x80066A34` starts
      the actual SortData stream at bucket **43**. The stream bucket is decremented in exactly one place —
      after an entity record (`0x800675E0`). The node path never touches it: it draws a whole node into the
      current bucket and stops the stream once the bucket falls below 4. So **the world's draw order is
      authored, and the ordering table only carries it**; buckets exist to interleave entities with the
      world at the right insertion point. Starting the stream at 45 shifts all world geometry by two console
      buckets and breaks its relationship with every structural and dynamic packet.
      Implemented in `src/formats/sortdata.[ch]`, unit-tested in `tests/test_surface.c` against an
      independent encoder (word-straddling fields, multi-word skips, both modes, unaligned starts).
      Checked disc-wide by `q2psx-inspect surfaces`, which **tiles** each chunk end to end — decode to the end
      opcode, round up to the next byte, start again — over all 115 chunks and 715,260 bytes: **8,968 streams,
      87 overruns at chunk tails, 178,801 node references and ZERO out of range.** A desynchronised bit reader
      does not land on a valid header 8,968 times in a row, which is what makes tiling a test rather than an
      illustration.
      **Opcode 1's branch is now exact too.** `0x80065D08` projects every point in the `f1` Scene/Points
      marker and the callee returns true iff both projected dimensions are non-zero; either collapsed axis
      is normalised to a literal zero rectangle. The result is clipped to
      parent `f3`: below-edge values clamp to the parent's minimum and above-edge values to its maximum.
      Hidden markers pass literal zero bounds. False skips `f2` bits; true
      treats non-zero `f2` as the signal for a new base. Forcing true globally was not harmless overdraw: a
      collapsed group then changed the bit position and made every later surface look like broken culling.
  - [x] 7a. Which stream a viewport starts at — **SOLVED directly; the equal-index inference is withdrawn.**
        The table pointer at `0x800C8E94` is the on-disc **PrimaryColl node array**, not a separate runtime
        record. `0x80066AD4` reads the camera cell from `view+146`, multiplies by the 36-byte node stride, and
        `0x80066AFC` loads signed halfword `+28`: the exact SortData **byte offset** for that cell. The client
        now locates each viewport camera in PrimaryColl and uses `node.sort_offset` directly. Stream tiling is
        retained for offline grammar/census work only; cell `i` never needs to be guessed as tiled stream
        `i`.
  - [x] 7b. What opcode 1 and Scene byte `+0x0E` build — **SOLVED directly.** PrimaryColl byte `+32`
        is the camera's seven-bit draw area. Opcode 1 projects its Scene/Points group and registers `f4`
        as another live area at the current authored bucket. Each projected rectangle changes the GTE origin
        and emits an in-list DRAWENV that restores the previous region when the OT later reaches that bucket;
        the world linkers cull against the current region-local extent. Each area is a 64-byte runtime record (table
        `0x800D8FE8` in NTSC): `+0` freshness tick, `+2` ordinal, `+4` screen-change record, `+8` Standard
        batches and `+12` Quick batches. Scene flags08 bit 14 builds a private packet
        chain keyed by `Scene[+0x0E] & 0x7F`; models use entity byte `+0x9E`, which movement fills from the
        occupied collision cell's byte `+32` at `0x80046B08`. The final drain at `0x80046E14` reconstructs
        `0x80047080`'s exact bounded dependency sort: newest-first Standard AABBs (maximum 32), reciprocal-
        edge suppression, midpoint splitting of intersecting boxes, Quick-to-Standard dependencies
        (maximum 128), stable signed-depth Quick runs, cyclic Standard selection, and minimum-edge cycle
        breaking. Area 1 instead concatenates at fixed slice bucket 46; area 2 sorts at fixed slice bucket 1;
        ordinary areas 3…95 require a fresh screen record. It then `CatPrim`s every whole private chain onto the area's authored bucket. A stale area
        is culled; global depth is not a fallback. The projection selector at
        `0x80065684` is stateful: negative returns untouched, 0 selects the
        global extent, 1 the viewport, and ordinary areas use viewport centre
        minus their region minimum. The view weapon writes area 1 at
        `0x8004EE58` before the normal model selector; using `-1` as an invented
        reset instead leaked the last particle/projectile region into the gun.
- [x] **8. `AreaConx` — SOLVED. It was never a 9-byte record, and the premise is what held it for three
      passes.**

      The payload was read as `n` interleaved 9-byte structs, and everything that followed from that is
      true: no byte offset gives a 1.3.12 unit normal in more than 39% of links, and the byte histograms
      show `0x10`/`0xF0` clustering that looks like halfwords sliced at the wrong parity. The record is two
      ARRAYS:

          u8   num_links
          u8   neighbour[num_links]              the adjacent area
          u8   pad                               only when (1 + num_links) is odd
          struct { s16 dist; s16 n[3]; }[num_links]

      and that IS the size identity this entry was written against: `1 + n + pad + 8n` is
      `9n + 2 - (n & 1)`, which holds on all 1,675 interior records. The formula was describing the layout
      the whole time.

      **What broke the premise is a test the interleaved reading cannot pass.** An adjacency graph must be
      SYMMETRIC. Taking the first `n` bytes as neighbours gives **3,494 of 3,494 edges with their reverse
      present — 100%** — against 25.6% for the best interleaved candidate. That pins the array boundary,
      and with it pinned the rest falls out at once: **3,494 of 3,494 normals are unit length in 1.3.12**,
      3,150 of them axis-aligned with a component of exactly 4096, and not one zero or otherwise. `dist` is
      non-negative, a multiple of 256, and runs 0..9984 — the plane's distance.

      So a link is a PORTAL PLANE to a named area, which is what an area connection graph needs and what
      the chunk's name says it is.

      Decoded in `q2_area_get_link`, and `q2psx-inspect verify` now checks the two facts that identify the
      layout on every link on the disc: **0 name a missing area, 0 carry a non-unit normal.** Either
      failing would mean the two-array reading is wrong.

      *The same mistake as #65, three entries apart.* Both stood on a premise that was never tested — "this
      address is a function", "this record is an array of structs" — and in both the searches that followed
      were sound, exhaustive, and answering the wrong question. A negative result about a wrong premise is
      indistinguishable from a discovery, and the only defence is to ask what would have to be true for the
      premise itself to hold.

- [x] **9. `SpaceLights` per-node partition. — SOLVED, and the partition was never in this chunk.**
      Two passes tried to split the array across the zone's **scene** nodes and got 0.68…7.12 entries per
      node with no rule that fit. The key is a **collision** node, and specifically a `SecondaryCol` one:

          lights of secondary node i = SpaceLights[ node[i].c_hi .. node[i+1].c_hi )

      where `c_hi` is the high halfword of the 36-byte node's field at `+28` — the field #23 recorded as
      unread because *"no instruction in the image loads offset +28"*. None does. The gather at
      `0x8006B0E4` loads offset **+30**, and its successor's at +66, exactly as the plane and link ranges are
      derived, which is what the totals sentinel was always there for. The entries are indices into
      `COMMON.DAT`'s `Lights` array (`0x800B2ED4`, stride 28, materialised at `0x8006B12C`).
      Checked disc-wide by `q2psx-inspect lights`: `PrimaryColl` carries the field on **0 of 115** zones —
      an independent confirmation that the engine's choice of hull is forced, not incidental — `SecondaryCol`
      is non-decreasing and starts at zero on **115 of 115**, and all **37,285** reachable entries name a
      light the map actually ships, **zero** out of range against light counts of 96…374. 13,805 nodes, mean
      2.70 lights each, max 45, 1,682 with none, and 15 zones (the front end, the intermissions and the FMV
      stubs) with an entirely zero partition. The 331 halfwords past the sentinel disc-wide are build
      residue the engine cannot reach.
      Implemented in `src/formats/spacelights.[ch]`; consumed by `src/game/lighting.c` and `flare.c`.
- [x] **10c. The `Population` place record's `id` and the item table. — SOLVED, and every item on the disc
      with it.**
      The id is not an index into anything on the disc either. It is the key into a **24-byte table at
      `0x8009F5CC`**, 64 records, scanned with `lb` and terminated by `0xFF`, and the reader is the item
      spawner at `0x800599DC` — which the level modules reach through import slot 11 of the block at
      `0x800B2FE4` (see #6). Each record names the item's MODEL, its BEHAVIOUR (a nine-bit flag word) and its
      EFFECT (an index into a 55-entry jump table at `0x800AC30C`, taken as `effect - 2`).
      What makes the flag decode CONFIRMED rather than plausible is the same class of argument the class
      table's health column carries: bits 4/5/6 are one dynamic-light channel each, and decoded that way the
      twelve key records spell out the colours their own names claim — Redkey R, Greenkey G, Bluekey B,
      Yellowkey R+G, Pkeypurp R+B, Whitekey R+G+B. Nothing in the decode was tuned to produce that.
      All 43 live effect handlers are disassembled and transcribed with the instruction each amount came from
      (`src/game/item.c`), including two asymmetries that are the original's and not the port's: **combat
      armour has no "already full" test** while body and jacket do, and INFINITE AMMO makes a weapon pickup
      grant its ammo type's *capacity* rather than a fixed amount.
      Twelve dispatch slots point at the failure exit, eight of them named by real records — the Xatrix/Rogue
      weapons, their ammo, the power screen and the stimpack. **No map on the disc places any of the eight**,
      which is the independent check that they are leftovers rather than a decode failure.
      `q2psx-inspect items` reads the table and the dispatch off the disc, diffs both against the port's
      transcription, then walks every place record: **1,013 of 1,013** resolve, **1,013 of 1,013** name a
      model the same map ships. Implemented in `src/build/itemtable.[ch]`, `src/game/entity.[ch]`,
      `src/game/item.[ch]` and `src/game/entitydraw.[ch]`; full decode in FORMATS.md §15.
      The three former residues are now closed:
  - [x] 10d. **The place halfword packs yaw plus three skill exclusions; bit 12 is unused authoring
        provenance. — SOLVED.** `0x8007F538` is the missing pre-spawn list walker. It reads the global skill
        at `0x800B334A`: skill 0 rejects `0x2000`, skill 1 rejects `0x4000`, and skill 2 rejects `0x8000`,
        naming NOT_EASY / NOT_MEDIUM / NOT_HARD exactly. Surviving records go to `0x800599DC`, whose generic
        spawner masks the low 12 yaw bits at `0x80058938`. No path tests `0x1000`; the census independently
        finds it set on every place in 22 maps and none in 12, with **zero mixed maps**, so it is a map-build
        marker with no retail gameplay reader. The port now filters both startup and CREBATCH place walks by
        the same global skill, while the all-record inspector deliberately bypasses the runtime filter.
  - [x] 10e. **The item table's final list names posed vertices for a subtractive floor shadow, not clips. —
        SOLVED.** `0x800599DC` stores `record + 0x10` in model-wrapper slot `+4`; its only consumer is the
        render-flag-bit-0 path at `0x800784CC`, after model drawing. That loop reads up to four halfwords until
        `0xFFFF`, maps each as a GLOBAL STORAGE vertex through `0x8006D608`, poses it through `0x8006C6C8`,
        and accumulates X/Z extrema. Item spawn leaves radius `+0x94` zero, so only the twelve records with
        two listed vertices produce a non-degenerate footprint. Rocket launcher's value `95` alone refutes a
        clip interpretation: the model has one clip, but storage vertex 95 is valid. The port now poses every
        item through clip zero, emits retail's RGB-128, ABR-2 `POLY_FT4` using the existing `chars.lbm`
        shadow tile, applies its `(600-height)/600` shrink, and reports emitted shadows in the headless census.
- [x] 10f. **`+0xFC` and `+0xFE` are two lighting-intensity factors, not geometric scales. — SOLVED.**
        The destination of the three `ScaleMatrix` calls at `0x8006B298` is `0x800DDD1C`, and
        `0x8006BBD4` installs that exact buffer with `SetLightMatrix`. The model rotation installed next is
        independently composed from entity `+0x2C0`; an exhaustive `access 0xFC` / `access 0xFE` sweep
        finds no geometric reader. The same product scales the ambient back colour at `0x8006B468`.
        `+0xFE` is not constant: the model-entity think writes
        `clamp(25 * (320 - time), 0, 4096)` at `0x8005A68C`, dimming an explosion without shrinking it.
        The allocator is the other entity writer and seeds both fields to 4096 at
        `0x8006C1B8..0x8006C1C0`. The port now keeps entity geometry at unity, sends both fields only to
        `q2_light_env_build`, and no longer culls a zero-intensity model.
- [x] **11. `MapMod` `Poly.uvIdxFlags` bits 6–7. — RESOLVED as a UV rotation; kept in Tier 0 above.**
- [x] **12. `Scene` node `flags08`. — SOLVED. It is four fields, not one bitfield.**
      The zone draw and the node emitter read the halfword at node `+8` four different ways, and the low ten
      bits are the piece that mattered most:

      | bits | meaning | read at |
      |---|---|---|
      | 0-9 | runtime object slot **+ 1**; 0 = none | `0x80067724`, `0x800665D4` |
      | 10-13 | the SETWIBBLE field; only 10-11 are read | `0x80066740` |
      | 14 | deferred, depth-sorted draw path | `0x80066524` |
      | 15 | do not draw | `0x8006771C` |

      **Bits 0-9 are the node-to-mover binding**, and their emptiness on disc is exactly why movers looked
      impossible from the data side: the loader clears them and a map's script fills them in. The value scales
      by 92 and indexes the runtime object array whose first element is `0x800D6BB0`. The object supplies more
      than the port was applying — the zone draw at `0x800678B4` calls `RotMatrix` on three `s16` Euler angles
      at `obj+0x0C` and then adds **two** independent `s16` triples, `+0x12` and `+0x18`. Rotating brush
      geometry (`ROTHATCH`, `SIMROT`, `ROTBUTTON`) is therefore drawn, not absent, and `mover.h`'s "there is
      no rotation anywhere in the engine" is true only of the linear `MOVER_A/B/C` integrator.

      **Bits 10-11 are a warp control.** They select one of four quad linkers — `0x800AF7CC`, `0x800AFC9C`,
      `0x800AFA2C`, or none — and what separates them is *when a near quad is subdivided*. Subdivision is
      `0x800B007C`: a 5×5 vertex grid (21 freshly projected, the four corners passed in as pointers) written
      into 16 `POLY_GT4` packets at 52-byte stride, i.e. one quad becomes a 4×4 mesh. Since UVs interpolate
      affinely, splitting a near quad sixteen ways is precisely what suppresses the texture warp — so
      "SETWIBBLE" names the wobble it controls. Variant 1 additionally rejects a quad whose vertex 1 or vertex
      3 projected to depth zero; variant 2 gates subdivision on `clut & 0x3C`; variant 3 links nothing, which
      is how a script hides a surface group. The decision is also budget-aware (`0x800698A0`): freely while
      the packet pool is healthy, only within a quarter of the threshold while it is half gone, never once it
      is nearly exhausted.
      Implemented in `src/formats/surface.[ch]` and wired into `src/game/world.c`; `tests/test_surface.c`
      pins the decode and `q2psx-inspect surfaces` checks it against all 17,035 nodes. Three predictions the
      reading makes are all confirmed: bit 15 is **never** set on disc, draw variant 3 **never** occurs, and
      the object field is **always** zero — the states a runtime-only field must not have been authored with.
      All 17,035 nodes fall inside the eight known values.
- [x] **4d. Rotating brush geometry. — SOLVED.** `mover.h` said "there is no rotation anywhere in the engine";
      that is true of the `MOVER_A/B/C` integrator and false of the engine. A node bound to a runtime object
      picks up a full rotate-about-pivot transform in the zone draw (`0x800678B4`–`0x8006793C`):

          node position = origin - camera - (R . p) + p + d

      with `R = RotMatrix(obj[0x0C..0x10])` (libgte, `0x80089E38`), `p = obj[0x18]` the **pivot**, and
      `d = obj[0x12]` the linear displacement. `R . p` is the 1.3.12 matrix-vector routine at `0x8006FB18`
      (three `mult`s summed, then `>> 12`). The `- R.p + p` shape is what identifies `obj+0x18` as a pivot
      rather than the "second independent displacement" an earlier pass called it. The missing constructor
      half is now implemented too: SIMROT/SIMROT2 (`0x800284C8`–`0x80028548`, `0x800286C0`–`0x80028740`)
      and ROTBUTTON (`0x8002C210`–`0x8002C288`) store the raw Scene-box midpoint minus Scene.origin.
      ROTHATCH (`0x8002B798`–`0x8002B830`) starts there, subtracts signed `item+10` from X and adds
      `item+12`/`item+14` to Y/Z. All three use the MIPS round-toward-zero midpoint sequence. Leaving these
      fields at the object's memset zero was why the native renderer rotated brushes around the wrong origin.
      The integrator is `0x8002F1A8`, reached from the 48-object sweep at `0x8002DC04`:

          if (!(obj[0x50] & 0x01000000)) return;
          obj[0x20] += (s16)obj[0x3A] * dt;
          obj[0x0C + 2*axis] = (obj[0x20] >> 8) & 0xFFF;
          obj[0x50] &= ~0x01000000;

      **It is one step per request, not a free spin** — the handler clears its own enable bit and `SIMROT`'s
      exec (`0x8002DEC8`) is what sets it, so continuous rotation is a script firing every tick. A port that
      implements a constant angular velocity spins doors that should have nudged. `axis = (obj[0x50] >> 14) & 3`,
      and since only `obj[0x0C + 2*axis]` is ever written, exactly one Euler angle is non-zero — which makes
      the matrix's composition order unobservable on this disc.
      Two operands were missing from the `SIMROT` table and are now recorded: **`item+4` is the angular speed**
      (`0x8002867C`) and **`item+20 & 3` is the axis** (`0x80028664`).
      Implemented in `src/game/rotator.[ch]`, applied in `src/game/world.c`, pinned by `tests/test_rotator.c`
      (49 checks). `q2psx-inspect surfaces` builds every `SIMROT`/`SIMROT2` on the disc: **11 rotators across
      49 maps, axis X 2 / Y 7 / Z 2 with the invalid axis 3 never occurring, speeds −3200…8000, and 0 node
      indices outside their map's zones.** Eleven is a small sample and is stated as such — it is consistent
      with the operand offsets rather than decisive about them.
      **All four rotation primitives are now built, and there are THREE integrators, not one.** Reading one
      family's arithmetic onto another turns a hatch 256 times too fast or a SIMROT eight times too slow:

      | primitive | handler | motion |
      |---|---|---|
      | `SIMROT`, `SIMROT2` | `0x8002F1A8` | `accum += speed*dt`; `angle = (accum >> 8) & 0xFFF`; one step per request |
      | `ROTHATCH` | `0x8002B460` | `angle += (speed*dt)/8` rounded **toward zero**, sweeping until it passes `obj+0x44` |
      | `ROTBUTTON` | `0x8002BFD8` / `0x8002C078` | a **snap**: the angle *is* 2048 when pressed and 0 when the hold expires |

      `ROTHATCH` runs inside a seven-state machine whose jump table is at `0x800ABDA0` — the same shape as the
      linear mover's. Its axis is a **byte at `item+8`**, not a halfword at `+20` as `SIMROT`'s is, and its
      speed's sign is chosen by which half of the circle the target lies in (`0x8002B70C`) so it always turns
      the short way. `ROTBUTTON` has neither an axis nor a target operand: its exec hard-wires `obj+0x0E`, the
      Y slot, and the literal 2048.
      Census over the disc: **33 rotators across 49 maps, axis X 4 / Y 25 / Z 4 with the invalid axis 3 never
      occurring, 0 node indices out of range**; 18 carry no speed, which is exactly the `ROTBUTTON` population.
  - [ ] 12a. Residue: `flags08` bits 12-13. SETWIBBLE writes a four-bit field (`0x8002E7CC` masks with
        `0xFFFFC3FF`) but the renderer masks to two, so values 4…15 alias onto 0…3. Bit 12 is nevertheless
        authored on disc — `0x1000` on one node and `0x1400` on three — and **no reader was located** for
        either bit anywhere in the image.
  - [ ] 12b. Residue: `unk0C` and `unk0D`. Scene `+0x0E` is solved as the seven-bit draw area (7b).
        `unk0C` (0…4, non-zero on 6 nodes) and `unk0D` (0…3, non-zero on 5) remain unread. Byte `+0x0F`,
        documented as "always 0", is scratch: the zone draw writes a per-frame counter into it at
        `0x80067A34`.

---

## Tier 3 — Behavioural / audio-visual polish

- [x] **12a. The effect system. — SOLVED, and it is four systems rather than one.**
      Recorded here because the shape of the answer is what made it tractable, and because two of the four
      were previously mentioned in passing as if they were the same thing.

      **The four.** *Particle groups* (`0x80030284`, drawn and integrated by `0x800304A8`): a pool of
      288-byte records, each holding up to fifteen screen-aligned quads that share one origin, one velocity,
      one acceleration and a pair of colour ramps. *Transient beams* (`0x80064E64`, drawn by `0x80064F10`):
      a 32-slot list refilled from empty every frame. *Debris* (`0x80064558` → `0x80064398`, think at
      `0x80064124`): real entities with physics. *Trails* (`0x80064C00` → `0x80064780`): a 96-vertex tube
      behind any entity carrying flag `0x04000000`.

      **A group is a burst, not a particle.** The fifteen quads cannot outlive each other, cannot be culled
      separately and all read the same ramp entry, so a burst fades as one object — which is why the
      original's explosions pulse rather than dissolve. Quad 0 is absolute; quads 1..14 carry an offset and a
      velocity *relative to it* (`0x800303C8` does the subtraction at spawn), so the burst translates with
      whatever made it while it spreads. The acceleration lives in what would otherwise be a fifteenth offset
      slot, which is why fifteen and not sixteen.

      **The ramp header is not a count, and the difference is visible.** Nineteen 132-byte records at
      `0x8009BA60`, ending exactly where a nineteen-entry pointer table begins at `0x8009C42C`. The leading
      `u16` reads 32 on sixteen of them — the entry count — but 64 on records 3, 4 and 13, which are still
      132 bytes apart. It is the semi-transparency field of a draw-mode word (`0x80030828` ORs it with
      `[0x800DDD5A]`): sixteen ramps add, three **subtract**. Ramp 3 is cyan and is the blood spray's second
      ramp; cyan subtracted from a lit wall is red, so a reader that took the header as a count would turn
      blood into a cyan flash and still pass every bounds check.

      **The ramp is indexed by age.** `colour[32 - life]`, so a 15-tick group only reaches the dim tail
      (entries 17..31) and changing a lifetime changes an effect's starting *colour*. The bright head of
      every ramp is unreachable for all but the 25-tick spark.

      **The beam hull folds rather than wrapping.** `0x800634E4` emits three axes 120° apart **and their
      negations, in that order**, so the six ring points come out at 0/120/240/180/300/60° and the six quads
      of the face list fold back through the beam's own axis. "Correcting" that ordering into a clean
      hexagonal prism produces geometry that looks nothing like the original at the same polygon cost.

      **Blood colour is a creature property.** `0x80059648` tests three flag bits — `0x10` red, `0x20` green,
      `0x40` blue — in that order and takes the first set. The chain has **no final else**, so a creature
      with none of them reaches the spawn with an uninitialised register: the same defect class as the
      `T_Damage` fifth argument, and handled the same way (the port picks a defined value and says so).

      All of it is checked by `q2psx-inspect effects` against the disc and by `tests/test_effect` without
      one. Full write-up in FORMATS.md §18.

      **The glint mesh is the `GlintMod` level chunk**, which was already in the schema as "OPTIONAL,
      present on BIGGUN only" and had never been connected to anything. `0x8007AB44` looks the name up and
      `0x800651BC` splits the chunk at a literal 864 bytes: 216 faces of four u8 indices, then vertices of
      four s16. The fourth halfword is the BAND COORDINATE, not padding — `0x80064938` reads `+6` off each
      source vertex. Two things confirm the split independently and neither was tuned for: on BIGGUN the
      chunk gives 218 vertices and the highest index anywhere is exactly 217, and the first vertex's band
      value is 1024, which is precisely the constant the renderer offsets its band centre by.

      **A particle quad is the 16x16 corner of `chars.lbm`** — the HUD atlas. `0x8001AD14` selects
      `[0x800DDD5A]` for the 8-pixel face and `0x80030830` hands the particle draw the same global, so
      uploading the overlay's font uploads the particles.

      **Debris moves in PrimaryColl at radius 2048**, not the player's hull: `0x80046DA0` installs
      `0x800C8E90` and the entity's `+0xA0`, where the player's path (`0x80046DDC`) installs SecondaryCol
      and `+0xA2`. Tracing shards against the eroded hull leaves them 286 units off the floor.

      **AND IT IS NOT A WEAPON TRAIL.** "A mesh dragged behind an entity with a band running along it" reads
      as a trail, and that is what it was recorded as until the decoded mesh was drawn: BIGGUN's 216 quads
      over 218 vertices are a SPHERE and the band sweeps its surface. Which means nothing in this engine
      draws a weapon trail at all — the blaster bolt is its own oriented hull and leaves nothing behind it.

      **What raises the glint flag is a LEVEL SCRIPT, not the engine.** Bit `0x04000000` of entity `+0x10C`
      gates the draw, and nothing in the executable sets it: all thirty-nine writes to `+0x10C` raise at
      most `0x8000`, and the only site in the image materialising `0x04000000` is the test itself. Read from
      the executable alone the glint is dead code. BIGGUN's `LevelBin` — a *relocatable MIPS module*, not
      bytecode — carries it at chunk `+0x1998` (`lw +0x10C; lui 0x0400; or; sw`) and tests it at `+0x1B00`,
      alongside `sb 6, 695(a0)` (the band count) and `sh 4, 702(a0)` (the phase). That 4 is the check that
      the band formula reads the right way round: `(width/4) * (4 - phase)` sweeps forward as the phase
      counts down, and the reversed subtraction looks identical standing still. The port therefore draws no
      glint by default — it cannot run relocated level modules — and puts the reconstruction behind a
      toggle rather than inventing an entity to wear one.

      **The particle CLUT is built-in palette 75.** `[0x800E3FC2]` is not a standalone global, which is why
      the address sweep found no writer: `0x80030DB8` materialises `0x800E3F2C` — the CLUT-id table the boot
      palette loop fills, indexed by record id — and reads `+150`. On this disc id 75 resolves to `0x3F0A`.

      **The glint has TWO draw paths** and only one had been reconstructed at first. `0x80064CE4` branches
      on the band count: zero uses the entity's own matrix, colour and phase at width 8192; non-zero walks
      that many 12-byte records at `0x800D565C`, each with its own angles, phase and colour, at width
      **4096**. The halved width is not cosmetic — by the shading formula the two uses of `width` do not
      cancel, so a multi-path band is narrower AND twice as bright. Both paths are now implemented. The
      phase is a plain byte decrement that UNDERFLOWS (`addiu 255`, no clamp, no reload), so refreshing it
      is the script's job and wrapping it here would be inventing a reload the console does not perform.

      **Debris gravity is READ, not modelled.** `0x80046464`: skip if `ent+0x10C & 0x2000`; otherwise
      `vel.y += [0x800AE924] * frame_delta` — the same global the player uses — clamped at **8192** on the
      way down only, which is what lets the burst's -1536 launch bias survive its first tick.

      **And the script can be READ rather than run.** That looked like a hard stop — `LevelBin` is a
      relocatable MIPS module and this project deliberately has no interpreter — but the question a port
      needs answered is not what the script computes, only whether it raises a glint and with what. So
      `q2_fx_glint_scan` looks for the raise triple and recovers the two immediates beside it. They are
      neither adjacent to their stores nor near each other (the phase's `addiu` is five instructions before
      its `sh`, and the sites are 0x168 apart), so each source is found by walking back for a load of the
      store's own register. On this disc that reads **6 bands, phase 4** out of BIGGUN and nothing out of
      the other forty-eight maps. The glint is therefore ON automatically wherever a script turns it on,
      with the script's own numbers — no interpreter and no special case.

      **AND THE GAME DOES HAVE A WEAPON TRAIL — the BFG's.** Two earlier passes here said it did not, on
      the grounds that every *particle* spawn site is an impact or a death. That is true and it is not the
      whole story: the trail is a BEAM. `0x800CABC0` is twelve 28-byte records walked every frame by
      `0x80048CA8`, which ages each timer by the frame delta and re-submits the live ones into the transient
      pool — the only effect in the game that outlives its frame without being an entity. `0x80049CF0`
      fills it from inside the BFG's own tick (`0x8004BD04`), behind a visibility test, with a timer of 45,
      a radius of 64 and beam style **3**. So the ball beams everything it can see and each beam lingers
      after it has passed. The dedup at `0x80049D30` is load-bearing: without it a ball in view of one
      creature fills all twelve slots in twelve frames and then stops drawing, because nothing frees a slot
      early. This also corrects the claim that beam styles 3 and 4 are unreachable — they are unreachable
      *from the laser dispatcher*, and reached from here.

      *Still open:* only WHICH ENTITY wears the glint. The script's search loop takes the record whose
      `+0xD2` is 46 and `+0xDA` is 20, over 48 entries of 768 bytes; the rule is recorded and the lookup
      becomes exact once the port's entity records carry those two fields. Nothing else is unread.

- [x] **13. Which in-game situation selects which music id. — SOLVED. It is the level's own playlist, and
      the id names a file and a channel.** Two tables, and neither had been read.
      The id at `gp+18524` (`0x800B2E5C`) indexes a **22-record, 6-byte table at `0x800A1DD8`**, with the
      stride spelled out at `0x80071760` as `(id*2 + id)*2`: `+0` is a signed file index into the five
      `QUAKE_x.XAI` names at `0x800A1E5C` — negative meaning silence, which is what ids 0 and 1 hold and
      which `0x80071778`'s `bltz` branches on — `+1` is the channel, and `+4` is the duration in **tenths of
      a second**. That last is checked rather than asserted: `q2psx-inspect music` demultiplexes and measures
      every stream on the disc and **19 of 20 agree exactly**, the exception being id 13 (QUAKE_C channel 3),
      whose table value is 175.1 s against a measured 176.1 — one second short, not a scale error, and worth
      leaving visible. Ids 2..21 are therefore the twenty (file, channel) pairs in file-major order.
      What WRITES the id is the cursor walk at `0x80071A68`, and the list it walks is in the **level table**:
      `p = cursor; cursor = p+1; v = *cursor`, where zero ends the list and a NEGATIVE byte is a relative
      jump back. A level record's `+0x22..+0x28` are seven track ids and `+0x29` is `0xF9` — minus seven,
      landing back on `+0x22` — so every level loops a seven-track playlist. The `s32` "always `0xF900 | n`"
      that leveltable.h recorded at `+0x28` was that seventh id, the loop byte and two zeros read as one
      word; and the "+0x22 varies without an obvious pattern" was the playlist's lead track. The seven are a
      lead and then six consecutive from lead + 3, wrapped into 2..17 — BASE0 is 14, 17, 2, 3, 4, 5, 6 —
      and ids 18..21, QUAKE_E's four channels, are in no level's list at all.
      Two records are not seven-and-loop and are what make the walk worth implementing rather than
      hard-coding: **QFRONT is one id then `-1`**, a single track looping for the title screen, and
      **MAGDEMO is four then `-4`**.
      `src/build/musictable.[ch]`, `q2_level_playlist_next`; the client plays its map's playlist and
      advances on end of stream.

- [x] **13a. (was 13) What, other than a stream ending, moves the playlist cursor. — SOLVED: nothing does.**
      The cursor at `gp+1536` has exactly **two** writers and both are now read.
      `0x80071B34` is the entry point: it clears `0x800B2710`, sets the music-enabled flag at `gp+1532`, and
      parks the cursor at `list - 1` — one before the first id, which is what the walk's pre-increment
      wants. It has a **single caller**, `0x800796C8`, and that caller passes `*(gp+18832) + 34`.
      `gp+18832` is the current level record (written at `0x8007C584`, at the head of the name lookup that
      resolves a level), and 34 is `0x22`. So the only thing that starts a playlist is a level load, and the
      list it starts is the level record's own — which corroborates the `+0x22` reading from the CONSUMER
      side, independently of the data's shape.
      The other writer is the walk itself at `0x80071A68`, and it runs when a stream ends. A zero byte ends
      the list and calls `0x80071B6C`, which stops the CD; a negative byte jumps back and loops.
      So there is no scripted music change: a level's seven tracks cycle for as long as the level lasts, and
      nothing in a boss room or a set piece can jump the cursor. That is a negative result about the game's
      design, not about the search.
- [x] **14. Does the engine loop XA tracks?** *(#104: no. It fades each in and out over 64 ticks and advances the playlist when the duration expires.)* The duration field is converted to 50 Hz ticks and stored to
      two globals with a 30.0 s fallback — that looks like a countdown to a restart or fade — but the
      consuming code was not disassembled. One entry is **1.0 s short** of its measured stream length, hinting
      the value is a deliberate restart point rather than a length.
- [x] **15. MDEC output depth for the movies (24-bit vs 15-bit). — the frames decode now (#115), and the
      question is answered from the other side.** The MDEC's own output is 24-bit RGB and the console has a
      24-bit display mode for exactly this; nothing in the bitstream chooses a depth, because the depth is
      the DISPLAY's. This port decodes to 24-bit and truncates to the framebuffer's RGB555 when it blits,
      which is a stated divergence marked at the blit: every other surface in this project is 15-bit
      because the GPU drew it, and a movie is the one thing the GPU did not draw.
- [x] **16. ~~Locate the movie player overlay.~~ — ANSWERED by #92.** It is a LevelBin module, and
      `QENDMIS1`..`QENDMIS5` are the maps that carry it. The filenames (`TAKE1BP.STX`, `OUTRO1P.STX`), the
      path pieces (`\Q2DATA\`, `MOVIES\`) and the MDEC decoder's own buffer names are all in there. The
      executable has none of it because none of it is in the executable.
- [~] **17. SPU RAM base / reverb work area,** *(#105: reverb is off — no mode fits. The RAM base itself is still unread.)* and whether reverb is disabled — the worst-case map leaves a
      **240-byte** margin against SPU RAM, suspiciously tight if a reverb buffer is also allocated.
- [x] **18. ~~`VramImageRec.width` / `height`: dimensions, or VRAM placement coordinates?~~ — RESOLVED by
      #1.** `width` is BYTES PER ROW of the decoded buffer, `height` is rows, and the VRAM rect is
      `width>>1` halfwords wide. Confirmed independently by `(width>>1) * height * 2 == decoded size` in
      553/553 records and by the allocator at `0x80068BAC` taking `mflo(mult(w,h))` with no shift.
- [x] **19. The secondary 512 × 256 display-env init function. — SOLVED, and it is dead.**
      It is not a second display configuration the game switches to: it is the first half of the boot display
      bring-up at `0x8006DFB8`, and **everything it writes is overwritten before it can reach the hardware**.
      The two `SetDefDispEnv` calls at `0x8006E03C` / `0x8006E054` set 512 × 256 rectangles, and four
      instructions later `0x8006E0C8` calls `0x80077540`, which rewrites both display envs to 512 × 248 at
      (0,0). The draw envs it fills — including the giveaway per-buffer debug colours (255,8,32) and
      (8,255,32), which would make a torn frame obvious — are rewritten every frame by `0x80076A74`. The
      single unrelated caller is the host-filesystem boot path at `0x8006E150`, which is also where the
      `c:\PsxData\Q2Data\` literal lives. **The game never displays 512 × 256**, and the port is safe to
      commit to one display model. Checked by `q2psx-inspect screen`, which asserts both the rectangle and
      the overwriting call.
- [x] **20. The `VSync(3)` path. — SOLVED. It is a CD settling delay, not a frame rate.**
      The site is `0x80069188`, inside the drive bring-up at `0x80069090`: poll `CdlNop` until the status
      clears, retry `CdlGetTN` until it succeeds, `CdlSetmode(0x80)` — double speed — then `VSync(3)` and on
      to `0x80071548`. Three fields is 60 ms on PAL, which is a mode-change settle, and nothing about it is
      periodic. The earlier note that it was "reached only through a function pointer" was a search artefact;
      the call is direct.
- [x] **42. What writes the screen shake. — SOLVED, and it is a whole screen effect: the water.**
      The shake at `view+780` had been transcribed from the per-viewport draw that *reads* it and had no
      writer, so the port carried a field nothing could set. It has exactly one writer in the image,
      `0x80062F90`, inside `0x80062DF0` — and that function is the **underwater screen effect**, called per
      view from the game update (`0x80038164`, `0x800384C0`) and gated on bit `0x100` of the owning entity's
      flag word, i.e. `UNDERWATER`.

      **What named it.** The pool of packets it draws out of is described in the executable's own allocator
      table as `"Water Moves"` (`0x800ACF7C`), with `"Used lots frame %d"` right after it as the overflow
      message. So the identification is the executable's own word for it, not an inference from what an
      underwater screen ought to look like.

      **What it does.** An amplitude at `view+776` ramps to 4096 at 24 per `dt` unit while submerged and back
      down at the same rate. From it come two more amplitudes (`amp*4095/1024` and `amp*4095/2048`), the shake
      (each of those `>> 12`, giving 0..3 and 0..1), and a pair of phase accumulators advancing at 5 and 7 per
      `dt`. Two run-length-encoded passes over the viewport then emit `DR_MOVE` packets — **VRAM-to-VRAM
      copies of the frame that has just been drawn** — displacing columns vertically and rows horizontally by
      `(cos(phase) * amplitude) >> 24` plus the shake. A semi-transparent tile of `(amp*15/4096, 0,
      amp*55/4096)` goes in last, so it is drawn first and the strips displace an already-tinted picture.

      **And it explains the shake.** The offsets are never negative, because the shake added to them is
      exactly the largest the cosine term can reach. The draw env insets the viewport by that shake and
      shrinks it by the same amount, so the frame is drawn with a margin around it and every copy reads from
      inside the viewport. The inset is not a shake at all — it is the wobble's margin. Nothing in the port
      could have worked that out from the read side alone, which is why the field sat there unexplained.

      This is also the first thing to need what `src/screen`'s header had flagged as unmodelled: the
      framebuffer read back as a source. `PSX_PRIM_MOVE` in `src/psx/gpu.h` is that seam. Full detail in
      FORMATS.md §12.6.4; `q2psx-inspect screen` checks 45 further constants against the disc (**174 of 174**)
      and `tests/test_screen.c` checks the behaviour, including the property the whole thing rests on — that
      no strip copy reaches outside its own viewport.

      *Residue, and it is small:* these tiles carry no texture page, so the hardware's blend mode for them is
      whatever the last polygon drawn before them left in the register and is not statically determined. The
      port picks `B + F` for both this and the damage flash, on the grounds that both fade to `(0,0,0)` and
      only a mode where zero is a no-op makes that fade continuous — and that the world's own selector table
      makes an unset selector additive anyway. Reasoned, not transcribed, and marked as such in `screen.c`.
- [x] **41. Which of the view weapon's two angle triples is the aim and which the kick.** *(#103: `player+230` is the aim; `0x80038260` returns the kick.)* The transform at
      `0x8004F40C` sums the triple at player+230 with the one `0x80038260` returns, negates x, and hands the
      result to `RotMatrix`. Nothing at the call site distinguishes them — both are three signed angles added
      to the same matrix — so `src/game/viewweapon.c` takes them as two inputs and adds them the way the
      original does. If the attribution turns out to be the other way round, nothing in the port changes;
      this is recorded so the ambiguity is not mistaken for a decision.
- [x] **39. The screen fade, if there is one.** *(#102: cut. Every module and every file on the disc searched; no reader and no data reference anywhere.)* `gp+16660` (`0x800B2714`) is set to **255** and `gp+16676`
      (`0x800B2724`) to **−16** at the top of every session (`0x8001834C` / `0x80018354`) — a fade level and a
      per-frame decrement if ever there was one — and **neither is read anywhere in the image**. Not
      gp-relative, not through a materialised base, and neither address appears as a data word. Either the
      consumer is in an overlay (the movie player, #16) or a relocatable module (#6), or the fade was cut and
      the writes are vestigial. The port implements no fade on this evidence. *Attack:* look for the readers
      once a relocated module can be disassembled — the same capability #6 and #10 needed.
      **Re-checked when the water effect turned up (#42), because a ramped full-screen tint is exactly what
      this item was describing and it would have been satisfying to close two things at once. It is not the
      same thing:** the water ramps `view+776` per VIEW by 24 per `dt` unit toward 4096, not a `$gp` global
      per SESSION by −16 from 255. Both fade addresses still have one writer and no reader. #39 stands.
- [~] **40. What SCREEN POSITION moves. — INERT ON THIS BUILD; the port honours it anyway.**
      The menu page writes `0x800B3368` / `0x800B336A` (defaults 0 and 24), and the sweep finds **no reader**.
      The obvious consumer would be the display env's screen rectangle, which `SetDefDispEnv` at
      `0x8008AD5C` explicitly *zeroes* — the game never fills it. So either the offsets are applied somewhere
      not in this image, or the page is inert on this build, and nothing here can distinguish those.
      That is the finding, and the port does not pretend otherwise about the console. It does, however,
      apply the offset **at presentation** — after the ordering table, where it shifts the finished image the
      way a television's own position control would and cannot perturb clipping or the viewport rectangles
      the reconstruction depends on. A control that exists and does nothing is a bug from the player's side;
      the default y of 24 is treated as the neutral point because that is what the reset routine writes.
      *Still open:* whether an overlay or a relocatable module reads them. Same capability as #6.
- [x] **20a. The menu system. — SOLVED, and it is data, not script.** Every menu is an array of 24-byte item
      records in the executable's data segment, walked by an engine at `0x80019B88`…`0x8001BA14`: the record
      is `{label, s16 x, s16 y, action, *toggle, *slider, on-release}`, the loader is `0x8001A474`, and a
      page is one or two of those arrays with the *last* one deciding what is navigable — which is how a
      page of pure text is expressed (a second call passing the empty table at `0x8009B30C`). All 17 page
      ids, the four GAME VARIABLES tables selected by cheat level, the two VIDEO variants selected by
      `0x800AEBCC`, the skip-on-`'g'` rule, the wrap, the fire-on-release flag, the toggle's
      LEFT-means-ON, the slider's two-units-per-held-frame over `0..127`, and the four colour codes
      `b/d/g/u` are all in FORMATS.md §10. Implemented in `src/menu/`, and
      `q2psx-inspect menu <disc>` reads the tables back off the disc and compares them: **21 pages,
      0 mismatches**.
      Still open: the MISSION screen belongs to the HUD system rather than the menu (#39).
- [x] **20d. The memory-card front end. — SOLVED for everything that is on this side of the hardware.**
      The screens are not pages: they carry no page id, are never handed to `0x8001A384`, and are installed
      directly by nine functions in `0x8001D2xx`…`0x8002040x`. But they are ordinary 24-byte item records, so
      the existing checker applies to them unchanged — **9 screens, 31 items, 0 mismatches** against the
      disc. SELECT SLOT, NOT FORMATTED (+NO/YES), FORMATTING, SAVE FILE, OVERWRITE? (+NO/YES), SAVE?
      (+YES/NO), SAVING, LOAD MESSAGE and NO CONTROLLER; full table in FORMATS.md §10.10.
      **Every action pointer in them is `0x8001FD80` — `jr ra; nop`.** That is the design and not a gap: the
      front end hangs nothing off the item callbacks and reads the cursor instead. The one exception is
      SAVE?, whose YES at `0x80020428` enters page **39**, parks `0x8001EFDC` as the per-frame handler and
      sets the mode flag at `0x800B32A2` — which is what starts the machine.
      The machine polls a state through `0x800B3234` and dispatches a **19-entry jump table at
      `0x800AB734`**; fourteen entries are the common exit, and the five live arms are 3 (hand the chosen row
      to `0x800B324C`), 5 (row 1 → 6, row 0 → 1), 13 (compose the cheat-level name), 14/16 (accept: apply the
      variables and leave) and 19 (row 1 → 20). All five gate on the **release** of CROSS, tested inline
      against `0x800B3290`/`0x800B3298` rather than through the record's flag bit.
      Implemented in `src/menu/memcard.[ch]`. The card I/O itself is three function pointers into `libmcrd`
      and is exposed as a host interface rather than invented.
      *Also corrected in the same pass:* the selection bar's suppression test at `0x8001A7FC` compares
      against `0x800AE634`, which is the **NUL terminating the previous string** — so the comparand is `""`,
      not the word at `0x800AE638`. An earlier reading of this pass had it as the label `PAUSED`; it is
      "a row with no text draws no bar", which is exactly what SAVE FILE's four empty slots need.
  - [~] 20e. Residue: which of the nineteen states shows which of the nine screens. Only two are settled by
        the arms themselves — state 3 hands over a row and SAVE FILE is the only row list, state 13 composes
        text and LOAD MESSAGE is the only composed screen. *Attack:* xref each of the nine table installers
        and follow its callers back to the state that reaches it.
        **The ENTRY POINT is now known**, from retail capture: the front end's MULTIPLAYER page carries
        `LOAD SETTINGS` and `SAVE SETTINGS` as its last two items, and those are what open the card screens.
        The in-game `SAVE?` prompt at `0x8009B3B4` is a second way in. So the machine is reachable from the
        menus rather than only from a level-completion flow, which is what an earlier note guessed at.
- [x] **20b. The menu FONT. — SOLVED, and the menu now draws with it.**
      The face is in two files already on the disc, and the answer is one function: the text drawer at
      `0x8001ACDC` branches on the drawable's `size` field at `+0x46` for 8, 16 and 32. Size 8 is
      **`chars.lbm`** — the same atlas the HUD draws from, through the same glyph table at `0x8009D554` —
      and sizes 16 and 32 are **`frontend.lbm`**, registered into VRAM slot 13 at `0x8003FE74` and landing
      at (896, 256), a whole 4bpp texture page.
      The 16- and 32-pixel faces are **not** table-indexed: `0x8001B494` computes a cell, letters in rows of
      15 (size 16, cells 16 x 11, v origin 100) or 7 (size 32, cells 32 x 20, v origin 0), digits on row 3
      and punctuation on row 2 through a jump table at `0x800AB564`, with four size-32 overrides so the big
      face carries `2`, `3`, `4` and `?` past the letters. Colours are the executable's built-in palettes —
      68/70/71/72 by size and by the drawable's `+0x48` highlight flag — modulated by a flat 128, or 32
      under the `g` code.
      **What made the placement checkable is the wider finding underneath it:** the standalone-image slot
      tables at `0x800A3274`/`0x800A329C` are twenty entries wide and cover standalone images exactly as
      they cover texture pages, which FORMATS.md had recorded as "not established". `0x8006901C(name, slot,
      v_offset)` → `0x800691A8` builds RECT{slotX, slotY + v_offset, width>>1, height}, and every UI image
      the game owns is registered through it in one function at `0x8003FE20`.
      Implemented in `src/menu/menufont.[ch]`, `src/menu/menudraw.[ch]` and `q2_vram_upload_named`; the
      menu now emits POLY_FT4 glyphs, a POLY_G4 selection bar and a LINE_F2 slider frame into the ordering
      table at the buckets the original names, and the client links it into the overlay slice before
      composition rather than blitting over the finished frame. Full detail in FORMATS.md §10.7–10.9.
  - [x] 20c. **`qk_menu.lbm` is the STATUS BAR's sheet, and the bar is now reconstructed.**
        The sheet registered immediately after the font — slot 14, (960, 256), and chosen by session mode at
        `0x8003FEAC` between `qk_menu.lbm`, `qk2_menu.lbm` and `qkm_menu.lbm` — decodes to a grid of
        **32 x 24 item and weapon icons followed by a set of large digits**. The digits matter, because the
        HUD reconstruction's headline result is that this game has no status bar (§11.1), and that result
        was reached by enumerating format strings — which large *bitmap* numerals would not appear in.
        What is established: `0x80033320` draws one cell as a POLY_GT4 from a four-byte {u, v, w, h} record;
        the icon rects are a five-byte table at `0x8009C478` ({u, v, w, h, id}, w = 32 and h = 24 on every
        entry but the leading 1 x 1 blank); a second table sits at `0x8009C658`; and the two callers,
        `0x80035B38` and `0x80035EA0`, sit inside a composite that runs five sub-draws in a row
        (`0x800352C0`, `0x80035554`, `0x80035B38`, `0x800359C0`, `0x80035EA0`).
        **UPDATE — it is the STATUS BAR, and §11.1 is retracted.** Retail screenshots settle it: the console
        draws a persistent bottom-of-screen bar with health, ammo and armour in large numerals beside their
        icons, and a weapon strip along the bottom right. The five sub-draws match its five parts. The
        cautious note this item used to end with — "this does not overturn §11.1" — was wrong, and wrong in
        the instructive direction: §11.1's method enumerated format strings and font-table readers, and a bar
        built from pre-rendered numeral sprites uses neither, so the method could not have found it and its
        negative result was an artefact of the instrument.
        **The bar's DATA is now read and checked**, in `src/build/icontable.[ch]`:
        the rect table at `0x8009C478` is **fifty-seven** records, not the 96 an earlier guess took it to be
        — the run of valid rectangles ends at `0x8009C595` and what follows is not rectangles at all. Its
        geometry is a 32 x 24 grid, eight per row, seven rows, **and the rightmost cell of every row is 31
        wide rather than 32**: `u` is 224 there and 224 + 32 wraps to zero in the u8 a primitive carries, so
        the narrow column is deliberate and a grid check that does not know it rejects one cell per row.
        Also read: the weapon-to-ammo-icon map at `0x800ABE9C` and its companion at `0x800ABEA8`, twelve
        bytes each on the same 1-based weapon id space as the weapon tables, with id 0 selecting the 1 x 1
        blank; and the split-screen size reduction at `0x800353C8` — 32 x 24 in single player, **24 x 18**
        for two and **16 x 12** for three or more. `q2psx-inspect hud` checks all of it: 57 records, 56 on
        the grid over 7 rows, 1 blank, 0 bad.
        **SOLVED — and the composite is not a screen.** `0x800337D0` is the **per-viewport draw hook**, the
        function the one-player layout stores in the view record at `+308` (`0x80033D30` and `0x80034288`
        are the two splits). The sweep never named its caller because there is no caller to name: the
        screen calls it through that pointer, once per viewport. So the bar is drawn by the same call that
        draws a viewport's world, which is why it is anchored per viewport and why its cells shrink in
        split screen.
        Its anchor is **view+304 / view+306** — the two halfwords screen.h carried as `pad_a`/`pad_b`,
        unknown. Seventeen 10-byte field records at `0x800337EC` give every cell a literal offset from it,
        and sorted by x they read **three digits then an icon, three times**, digits 24 apart:
        A at −71/−47/−23 with its icon at +0, B at +64/+88/+112 with +135, C at +179/+203/+227 with +250.
        Field 12 at +330 is the one-player auxiliary icon filled by `0x80037CAC`; its precise gameplay
        meaning remains open, but it is not the split-screen frag counter.
        The **numerals are at `0x8009C598`** — ten four-byte {u, v, w, h}, all 24 x 24 at v = 168, u = 24 *
        digit, followed by the minus sign at (0, 192) and the 1 x 1 blank. That 24 is the same stride the
        one-player field table uses. The call sites now prove the group order directly: `0x80035178` gets
        health, `0x800352C0` ammo and `0x80035554` armour; capture independently agrees.
        Implemented in `src/game/statusbar.[ch]`, wired into the client's per-viewport draw and fed the
        sim's real health, armour and current-weapon ammo. Full detail in FORMATS.md §11.1.1.
  - [x] 20f. **The icon vocabulary and the quad layout.** Each record's fifth byte is its **palette index**,
        copied to byte +8 of the ten-byte field record; it is not an item-effect id. All three status-bar
        sub-draws select a rect by index (`base + index * 5`) and none scans or compares that byte. Pickup
        captions use the item effect as a rect index directly, while health and armour use the hard-coded
        rect indices proved at their reader arms.
        **All four installed hooks are now read — SOLVED.** The selector is the proof that corrected the old
        labels: `0x800337D0` is one player (17 fields), `0x80033D30` two stacked (16), `0x80034288` two
        side-by-side (16), and the real three/four-player hook is **`0x80034830`**, not `0x80034288`.
        The side-by-side hook puts armour and signed frags on its upper row with the literal expression
        `anchor_y + 40 - framebuffer_height`. The real quad hook builds **11 fields per viewport** from 44
        x halfwords at `0x8009C600` (`view * 11 + field`) and four y halfwords at `0x800AE808`:
        `{110, 110, 1, 1}`. Each viewport carries its own health, ammo and signed frag readout; armour is
        omitted. Views 1 and 3 slide one- and two-character frag values against their inner edge.
        The client now selects these exact tables, feeds each viewport its owning player's parked combat
        state and frag count, and omits the one-player pickup/powerup/weapon-strip sub-draws from the three
        split hooks exactly as their call graphs do. Headless 2H capture shows player 0 at 100 health and
        player 1 at 84 in separate panes; quad capture shows four bars along the central split.
        `src/game/statusbar.[ch]`, `q2_sbar_fields_2h`, `q2_sbar_fields_2v`,
        `q2_sbar_fields_quad`; `tests/test_statusbar.c`.
- [x] 46. **The deathmatch scoreboard.** *(#101 reads its static page and recovers the six mode titles; #106 puts the prompt where the module puts it. The per-player rows are runtime, not records, which is the answer rather than the gap.)* Capture shows a `DM SCORES` screen: a title on a **red** bar rather
      than the usual blue, one row per player carrying that player's own bar colour (RETRODAN red, PLAYER 1
      blue) with a name and a frag count, a `READY` marker on the left of a row that has pressed fire, a
      backdrop image, and a centred `ALL PLAYERS PRESS / FIRE TO CONTINUE` in the 16-pixel face. Every part
      of it is machinery this project already has — the bar, the two faces, the per-player colour — so what
      is missing is the screen's own table and flow, not anything that draws it.
      It also shows the title bar is **not always blue**, which §10.8's four-colour table already allows
      (red is index 1) but which nothing had confirmed for a title.
      **What this pass added.** The scoreboard is a **level**, the same way the front end is: level-table
      record 12 is `MPResults` -> `LEVELS/QMRESULT/`, on the disc, whose `ModelNames` are `Q2LOGO`, `q2title`
      and the four coloured player models `Male2`, `Male2aqua`, `Male2purple`, `Male2red`. That is a direct
      confirmation of §10.8's per-player colour table from a second, independent place — the four colours
      exist as four separate model assets — and it explains the backdrop the capture shows. The rows
      themselves are menu items over that scene, so the missing piece is the same run-time item construction
      as #44 rather than a table to transcribe.

---

## Tier 4 — Low-impact unknowns

- [~] 21. `CastList` blocks A, B, C and D — **structure now known, semantics not.** The load-time rescale at
      `0x8006C214` reveals all three of B, C and D:
      **B** (`+0x30`, never relocated) is 8 `u16` chain heads — which is what "exactly 16 zero bytes on
      821/965 models" always was, eight empty chains — with nodes `{u16 countAndFlags; u16 next; entry[count
      & 0x7F]}` and entries of two `u16`, both multiplied by **10** at load.
      **C** is a chain of records whose `s16` at `+0` is multiplied by **10** and whose word at `+4` is the
      byte delta to the next.
      **D** is 20-byte records ending at a zero word, with three `u16` at `+12`, `+14`, `+16` multiplied by
      **5**.
      The vertex array is NOT rescaled, so model vertices are already at world scale. Still open: what the
      values mean, block A's payloads, the animated-model frame layout, and the header's 24-bit field at
      `+0x01` (261…333367). **Block C is not a vertex-base candidate** (#2a).
- [~] 22. `PrimaryRemap` value space — **narrowed by four measurements, and "probably a polygon id" does not
      survive the first of them.** The max exceeds the scene node count in 100 of 115 files, which is where
      this entry started. Measuring the values rather than their maximum:

      1. **They are DISTINCT within a zone — 115 of 115.** One id per collision node, never repeated. A
         polygon or surface CLASS id would not need that; an identity would.
      2. **They repeat ACROSS the zones of a map.** BASE1's zones use 358 distinct values over 371 nodes,
         BASE2's 404 over 434. So two zones can name the same id, which is what a boundary shared by a
         zone cut looks like.
      3. **Neighbouring nodes usually get neighbouring ids.** On BASE0 zone 0, 109 of 186 successive
         differences are exactly +1, and the sequence is not sorted — so it is an ordering the zone
         inherited, not one it was assigned.
      4. **The value space is bounded by the map's quad count in 115 of 115** and by the zone's own in 107
         of 115, but the max only reaches about a quarter of either on average.

      Two hypotheses were tested and REFUTED. The values do not fall into a per-zone slice of the map's
      quads concatenated in zone order — 47 of 115 — so it is not an index into a map-wide polygon array
      laid out that way. And the map-wide maximum equals the map's quad count minus one on only 15 of 49
      maps, so it does not span one either.

      What fits all four is an id the zone INHERITED from whatever it was cut out of — an authoring
      cross-reference rather than a runtime index — and the last measurement supports that: the pointer is
      installed at `ctx+0x10` by `0x80051798` and **nothing in the collision code reads offset 0x10 of a
      context**. This port implements the whole collision model without it and walks 47 of 47 maps.
- [x] 23. `CollNode` fields `c` and `d`. **Both runtime meanings are SOLVED.** `d`'s low byte is the node's **contents id**,
      read with `lbu +32` at `0x80044DB8` — where a trace records it into its contact list whenever it
      changes along the path — and at `0x8004510C`. The other three bytes are zero on all 22,773 nodes.
      **`c` (+28) is two hull-specific halfwords.** The high one — bytes +30..+31 — is SecondaryCol's first
      index into `SpaceLights` (#9), read at `0x8006B0E4`. The low one is PrimaryColl's exact SortData byte
      offset, read with `lh +28` at `0x80066AFC` after the viewport camera cell indexes that hull. Reading
      them as one 32-bit quantity made the old maximum look like noise. The unused opposite-hull copies need
      not be monotonic: no located consumer reads PrimaryColl's light half or SecondaryCol's sort half.
- [~] 24. **`Resources` `unk0` and `unk4` are authored constants, not derived budgets — measured against
      six properties of the zone each one names and correlated with none of them.** Over all 115
      record/zone pairs:

          unk4 vs scene nodes 0.07   coll nodes 0.03   areas 0.13   points 0.09   sort 0.00   lights 0.17
          unk0 vs scene nodes 0.20   coll nodes 0.27   areas 0.26   points 0.16   sort 0.24   lights 0.20

      Every |r| is under 0.27, including against the zone FILE SIZE (0.09 and 0.17). So "per-zone budget or
      limit" in the sense of something computed from the zone's contents is refuted: whatever these are,
      they were typed in rather than measured out. The port loads all 115 zones without reading either.
      Original text follows.

      `Resources` `unk0` (−3000…6600, 49 distinct) and `unk4` (40…180, 17 distinct); `unk3` (64, but 80
      in two records).
- [~] 25. `TrigBounds` trigger `id` (9…75 plus 255, where 255 is "none") and `flags` (14 distinct values up
      to 10240). **Three flag bits are now known**, read out of the contents test at `0x80050CE0` and the
      player integrator that consumes it: `0x0200` makes an entity **sink slowly** (vertical velocity eased
      toward +1024, decelerating by `dt*24` above it), `0x2000` makes it **buoyant** (eased toward −3072,
      plus a −9216 impulse when it is already on the ground — the water boost), and `0x1000` gates the
      entity's own `0x800` flag on `|vel.y| < 1024`. The mask the player passes is `0x3360`, so `0x0100`,
      `0x0040` and `0x0020` are also volume classes it cares about and their effects are not yet traced.
      The same test doubles as the entity-overlap query, which is how a trigger volume blocks movement.
- [x] 26. The five `Lights` style values (`(n<<3)|7` for n = 0…4). — **SOLVED. They are LENS FLARES, and the
      byte is three fields, not one.** The flare pass at `0x80075708` splits `type` as: bits 0–2 (always 7,
      never read), bits 3–5 the **flare style** 0…4, bits 6–7 a **size shift** giving a screen reach of
      `64 << k`. Style 0 draws nothing — the per-viewport pass at `0x80075AA4` masks the word at `light+16`
      with `0x3800` and skips the light outright — so a style-0 light lights the world and is never itself
      seen. Styles 1…4 index four NUL-terminated element lists at `0x800A1FDC`, `0x800A2014`, `0x800A2024`
      and `0x800A203C`, whose 8-byte records are `{kind, size, position, colour}` with the position a 1.12
      factor **along the line from the screen centre to the light** — which is what makes this a lens flare
      rather than a corona. The four lists are nested: style 2 is style 3's first element, style 3 is style
      4's first two, style 4 is style 1 less its last. Disc-wide: 6,183 lights at style 0, 660 at 1, 183 at
      2, 2 at 3, 786 at 4; every light on the disc has size shift 0.
      Both element kinds are untextured and additive — a Gouraud fan bright at the centre and black at the
      rim, plus eight Gouraud lines for the starburst — so the long search for a flare texture was looking
      for something that does not exist. Full detail in FORMATS.md §17.3; implemented in
      `src/game/flare.[ch]` and checked element-for-element against the executable by
      `q2psx-inspect lights`.

      **And the whole of it has now been read down to the last constant, which turned up four things the
      first pass had left standing.** *(1)* The four "magic multiplies whose closed form was not recovered"
      are all exact integer divides: a magic `M` with post-shift `p` is a divide by the unique `d` with
      `M == floor(2^(32+p)/d) + 1`, and all six of the flare's divides solve to `320` or `240` times 4096,
      2500 or 3500. The starburst's proportions are `4096/2500` and `4096/3500`, which is what the earlier
      pass measured as 1.637 and 1.170. *(2)* The DISC does not use the n-gon generator at all — `0x80074C4C`
      is its own unrolled hexagon, reading one table entry and mirroring it. *(3)* The translation vector at
      `0x800DDD7C` was recorded as an untraced substitution; it has no writer anywhere in the image, sits in
      BSS, and two of its three readers hand it to `T_Damage` as both `dir` and `point` — it is a static
      zero vector, and zero was right for a reason rather than by luck. *(4)* The generator's mirror index is
      `out[n+1-k]`, not `out[n-k]`; this port had the latter, which turns the 12-gon into an 11-gon with a
      doubled vertex. `tests/test_light.c` now counts the distinct rim vertices, because a screenshot cannot.

      **The pass also had no caller.** `q2_world_zone` carries `lights` and `light_node` for exactly this and
      nothing ever assigned them, so `q2_world_build_ot` took the null branch every frame of every run and
      not one flare has ever been drawn by this port. Wired in `src/client/main.c` and in the offline
      renderer, and both now report a per-frame flare census — considered, styled, culled near, culled dark,
      drawn, primitives — because "0 drawn" and "6 drawn but all off-screen" are different faults.
- [x] 27. **Both halves are answered, and the first was answered without anyone marking it.** The entry
      asked for the flag bits beyond 0, 1 and 8 and for the `extra` list's meaning.

      **The flags are all named.** `q2_item_flag` now carries nine bits, each with the address that reads
      it: SPIN (0x8005 9458), MATERIALISE (0x80059488), TIMED (0x800597C8), OBJECTIVE, the three GLOW
      channels (0x80059648..0x8005973C), NO_ANIM (0x800593F4) and NO_SPAWN_ARG (0x80059A44). Measured over
      the whole table, the union of every flag word set anywhere is **0x01FB**, and the bits it contains
      that no `q2_item_flag` names are **0x0000** — `q2psx-inspect items` prints both. Nothing is left to
      name. (0x0004 is absent from the union, which matches TIMED's own note: the runtime sets it on items
      a dying actor drops, and no authored record carries it.)

      **The final list is a set of posed SHADOW VERTICES, not clips.** The spawner does store
      `record + 0x10` into model-wrapper slot `+4`, but the only consumer at `0x800784CC` treats its
      `0xFFFF`-terminated halfwords as global-storage vertex indices. It maps and poses each vertex, grows
      X/Z extrema, applies yaw and the entity base, then emits a subtractive `POLY_FT4` from the lower-right
      16 × 16 tile of `chars.lbm`. Every shipped item model has one clip and plays clip zero; values up to 95
      in this list could never have been clip ids. This correction and the packet path are covered by
      `tests/test_item.c`, while `q2psx-inspect items` checks all disc placements and prints the vertex census.
- [x] 28. **All three clauses answered, and two of them were answered elsewhere without this entry being
      updated.**

      **`+0x1C` is a per-level FUNCTION POINTER that every level fills with the same stub.** It reads
      `0x800412C8` in all 52 real records — 52 raw words in the image equal that address, which is the
      table itself — and `0x800412C8` is `jr ra; nop`, an empty function. That **refutes** this entry's own
      guess that it might be the high half of an 8-byte field: `+0x18` is zero in every record and `+0x1C`
      is a valid code address, which is not how a 64-bit quantity is laid out.

      Nothing on this disc invokes it. Three sites in the image load `+0x1C` and call through it
      (`0x80056B74`, `0x80083A6C`, `0x80085C98`), and the first — the only one in the game code — walks a
      **48-byte** stride table and hands the callee an entity, so it is not this 56-byte record. A per-level
      hook slot, filled with a stub by every level: the same shape as `EXPLO`, which is a pure stub in both
      its binding-table slots.

      **`music_playlist` was read a long time ago and this entry never caught up.** "No instruction loads
      offset 0x22 from a level record" is true and is not evidence of anything: the player holds a CURSOR.
      `0x80071A68` walks it — a positive byte is a track id, a negative byte is a relative jump back, a zero
      ends the list — so the loads are `lb 0(cursor)`, never `lb 0x22(record)`. `leveltable.h` has carried
      that reading, and the port plays every map's seven-track list from it, with 19 of 20 stream durations
      matching to the tenth of a second.

      **`runtime[8]` does not exist.** The record is 56 bytes and the layout accounts for all of them —
      12 + 12 + 4 + 4 + 2 + 7 + 1 + 2 + 12. The field this clause asked after was a artefact of an earlier,
      wrong stride.
- [x] 29. **~~`SNDVRAM` section A header bytes `0x0E` and `0x0F`, and the split.~~ — FULLY RESOLVED.**
      They are 4bpp CLUT counts (the fix-up loop at `0x800762B4` adds them, shifts left by 4 and sets the STP
      bit over exactly that many halfwords), and the **split is world versus models**: the world renderer
      indexes the CLUT array from zero, while the model emitter at `0x8006A3FC` adds `count_a` first, so a
      `CastList` face's `texture` byte addresses section B. Checked disc-wide — no world polygon indexes past
      its map's `count_a`, and none of the 138,290 model faces has `texture >= count_b` (max 180 against a
      largest `count_b` of 181). The engine using only the sum to size the upload is why the split looked
      vestigial from the data side.

---

## Tier 5 — Archival / other-build / process

- [!] 30. **TERMINAL WITHOUT THE DISC — and what is needed is small and listed.** Every value here is a
      property of a release this project does not have a copy of, so no amount of work on the PAL disc
      produces one. What makes it terminal rather than open is that the port is already structured for the
      answer: builds are identified by EXECUTABLE HASH rather than by region (`src/build/`), so an NTSC
      disc supplies its own row and nothing else has to change. The list below is that row.

      **NTSC build values:** framebuffer height, `video_mode_const`, movie filename suffix, EXE hash,
      PVD fields. All must be **read**, never guessed — PAL turned out to be 512 × **248**, not the widely
      assumed 256, so the folklore 512 × 240 NTSC figure is *less* trustworthy now, not more.
- [~] 31. Real xrefs to the `.DAT` chunk-name literal pool at `0x800AD414` — **found, and the pool is not
      indexed.** The zone loader at `0x8007B3F8` names each chunk by materialising its 12-byte literal
      directly (`0x8007BA78` for `MapMod`, `0x8007BB74` for `Points`, and so on), copying it to the stack and
      calling the directory search at `0x8006DBC0`. There is no per-chunk flag word and no table walk, so
      required-vs-optional is expressed only by what the loader does with a failed lookup. Still open: that
      per-chunk failure handling, chunk by chunk. The same pass mapped each chunk to the global its pointer
      lands in — `Scene` `0x800B2C3C`, `MapMod` `0x800B2C6C`, `Points` `0x800B2CA0`, `SortData` `0x800B2C84`,
      `SpaceLights` `0x800B2ED0`, `AreaConx` `0x800B2D1C`, `PrimaryColl` `0x800B2E0C`, `MapNames`
      `0x800B2C9C` — which is what makes each chunk's consumer findable with `xrefs`.
- [x] 32. **`ModelNames` is tool-only data, and the proof is that this port has never read it.** The chunk
      is present in all 49 `COMMON.DAT` files and the string appears zero times in the executable, which
      left three hypotheses: dead data, positional access, or a runtime-assembled name.

      Its layout, measured: a flat run of 12-byte NUL-padded names, padded to the container's 4-byte
      multiple — 49 of 49 maps, **1,407 names**. BASE0's are `Debris1`, `Debris2`, `Debris3`, `Chest`,
      `HelpComputer`, `Blaster G`, `Shotgun G`, `Gib meat` and ten more.

      **It is redundant.** `model.h` already records that a CastList model carries its own `name[12]` at
      `+0x08` and that it appears in this map's ModelNames on all 49 maps. So the engine has every name it
      needs inside the model bank, which is why it never has to look this chunk up — and why its name is
      not in the executable to look up with.

      The port is the check: `Q2_COMMON_MODEL_NAMES` appears in the chunk-name table in `level.c` and
      **nowhere else in `src/` or `tools/`**. Every model on the disc — 1,723 of them — every creature's,
      every item's and the view weapon's, resolves by the CastList name with this chunk unread. A
      reconstruction that works without it is the strongest statement available about whether the engine
      needs it.
- [x] 33. **`TriggerRemap` and `SecondaryRem` are emitted by no file, and the port would have REFUSED one
      that shipped them — which is the part worth fixing.** A census of all 164 containers finds exactly
      25 distinct chunk names and neither of these is among them, confirming the original observation.

      Which of "cut" and "another source" is right cannot be settled from a disc that carries neither. What
      could be settled was this entry's own recommendation — *a parser should tolerate them appearing* —
      and this port did not: `resolve()` REFUSES an unknown chunk name outright, on the reasonable grounds
      that misreading an unfamiliar schema is worse than failing on it. The consequence was that a release
      emitting either chunk would not open at all, and the failure would read as an unsupported build
      rather than as two extra chunks.

      Both names are now optional zone slots. They cost two enum entries, they are absent on this disc, and
      a build that ships them loads with them simply unread. `verify` still passes over all 164 containers,
      which is the check that the change costs nothing here.
- [x] 34. **The permutation is three states keyed to zone index, stated exactly — and only three chunks
      ever move.** "Perfect and exceptionless correlation" was the observation; this is the rule:

          zone 0    SecondaryCol  PrimaryRemap  AreaConx      49 zones
          zone 1    PrimaryRemap  SecondaryCol  AreaConx      33
          zone >=2  PrimaryRemap  AreaConx      SecondaryCol  33

      Exceptionless over all 115 zone files. It is not a rotation by index — it is three arrangements with
      everything from zone 2 up sharing one.

      **And the whole directory has only FIVE distinct orders**, over 115 files: every chunk except those
      three sits in a fixed position, and the two remaining variants are the same three arrangements
      without `CreAIRel`/`CreAIBin` on the zones that carry no creatures. So the variation is entirely
      those three names and nothing else.

      Ordering by SIZE is refuted — 82 of 115 zones have the trio in neither ascending nor descending size
      order, and the 33 that are ascending are exactly the zone-1 group, which is that arrangement's
      coincidence rather than a rule.

      The build tool's internal reason is not on the disc and cannot be. What matters for a parser is, and
      it is now exact rather than "unsafe in general": **look chunks up by name**, which this port does.
- [!] 35. **`MAP.ALL` — TERMINAL: the disc cannot answer this and the answer it does give is "nothing reads
      it".** Its purpose, its four header words at `+0x40` (three decode as plausible floats near
      −1.93…−1.97) and whether the 16-entry table length is right are all unverifiable at N = 1. What IS
      established points one way and only one way: the filename appears **zero** times in the executable,
      and the file's last 44 bytes are MSVC uninitialised-heap fill — a buffer written out longer than the
      data in it. Both are the signature of an editor leftover shipped by accident. Kept as a standing
      note rather than an open question, because no further evidence exists on this disc to gather.
- [!] 36. **TERMINAL, and the entry says so itself: both are zero in 100% of samples.** The unused 20-byte
      tail of every Form 2 payload and the always-zero `uint16_t` at `+2` of each music table record.

      The tail is not even a disc question: a CD-XA audio sector's 2,324-byte Form 2 payload carries 18
      sound groups of 128 bytes — 2,304 — and the remaining 20 are unused by the standard, which is why
      `xa.h` warns that a decoder consuming 2,324 will drift. Zero in all 70,663 audio sectors is the
      standard being followed, not a field withholding its meaning.

      The music record's `+2` has no such external explanation and no internal one either: a field that is
      zero in every sample carries no information about itself. Kept as a standing note. Nothing further
      can be gathered here.
- [x] 37. **`GlintMod` is a MESH, decoded and drawn — "high-entropy after the first few dozen bytes" was
      the description of an index array read as noise.** The chunk splits at 864 bytes: an index array of
      four bytes per face, then an `s16[4]` vertex array of `{x, y, z, band}`. BIGGUN's 2,608 bytes are
      **216 faces and 218 vertices, highest index 217, bands 1024..31744** — `q2psx-inspect effects` prints
      exactly that, and `q2_fx_glint_mesh_decode` is the reader. The entropy was the low, slowly-rising
      face indices of a mesh, which is what a run of small increasing bytes looks like to a histogram.

      Its two draw paths, its band phase and what raises the `0x04000000` flag are all in `effect.h`; only
      BIGGUN carries one, and the level script that turns it on is that map's own.
- [x] 39. The HUD's residuals, none of them blocking, now that the overlay itself is done. **CLOSED: every one of them below is answered, and the last is answered as a CUT feature rather than a missing one.**
      **~~The MISSION / level-completion screen.~~ — RECONSTRUCTED, and it draws.**
      `0x80021ADC` builds it with `sprintf` into a scratch buffer at `0x800C6EE8` and hands each row to the
      text printf at `0x80043518`, into the **overlay camera's** context at `0x800D6870` rather than into any
      viewport. Its rows are three centred body lines from the runtime buffers at `0x8009B5E8`, `0x8009B608`
      and `0x8009B634` — starting at y = 36 and stepping **+10 then +8** — followed by a label column at
      `box_x + 20` reading `Location`, `Secrets`, `Kills` and, after a further gap, `Totals:`, with a value
      column to their right. The text box is `gp+352`…`gp+358`, and each body line is centred by the helper
      at `0x80022550` against `left` and `left + width`, measured with the HUD's own off-by-one measurer.
      **The format strings are markup, and that is the confirmation.** Every one begins `@%03X%2X` or
      `@%3X%2X` — the `@XXXYY` pen escape with its three hex digits of x and two of y, formatted in at run
      time. Finding the game assembling its own escapes with `sprintf` is independent evidence that the
      markup reading in §11.4 is right, since the formats only make sense if the interpreter reads them that
      way. Three colours carry the whole visual grammar: `BEF0E6` for labels and the title, `DCF082` for
      values, `C8F000` for totals. The title is `"Mission  %d  -  Complete"` (`0x800AB9DC`).
      **A bound that falls out of the formats.** Label rows use `@%03X`, value rows use `@%3X`. The escape
      reads exactly three characters as hex, so the unpadded form only produces three hex digits when
      x >= 0x100 — below that `sprintf` pads with a space, the space is eaten as part of the field, and the
      pen lands elsewhere. Using the plain form for every value row and the padded form for every label row
      is only consistent if **the value column sits at x >= 256**. That is a proven interval, not taste, and
      the port's constant is named for it.
      Implemented in `src/game/mission.[ch]`. The counters are inputs rather than something the module
      invents, because kills and secrets are simulation state the sim does not yet tally — a caller that has
      counted nothing gets a screen saying zero rather than a screen that lies.
  - [ ] 39a. Residue: the y step between the label rows and the value column's exact x. The rows are emitted
        with individually incremented values rather than from a table, so pinning each needs another pass
        over `0x80021CC0`…`0x80021ECC`. Both are marked `_UNREAD` at every use.
      **~~The split-screen overlay.~~ — RESOLVED, and there is no split-screen overlay.**
      `multipics.lbm` and `multipic2.lbm` are not overlay art. Decoded, they are **deathmatch map preview
      thumbnails** — ten in the first and two in the second, in a 2 x 5 grid of roughly 128 x 51 texels — and
      they live in `QFRONT` alone, not in any playable map. They belong to the front end's multiplayer map
      select, which is a screen this port does not have, and nothing in the in-game overlay draws them.
      The same pass decoded their two neighbours, which are the same kind of thing: `control.lbm` and
      `mouse.lbm` are **controller diagrams with callout lines** — a DualShock over a standard pad, and the
      same pad over a mouse — for the CONTROLLER page to label. Also `QFRONT`-only.
      What made all four readable is that they are **8bpp**, which nothing had established. The geometry
      forces it: the upload rect is `width >> 1` halfwords, a texture page is 64 halfwords, and a primitive's
      `u` is eight bits, so an image 128 halfwords across is 512 texels at 4bpp and cannot fit a page, while
      at 8bpp it is 256 and fits exactly. Decoding confirms it — at 8bpp `multipics.lbm` is ten recognisable
      screenshots and `control.lbm` is a photograph of a controller; at 4bpp both are noise. Depth is
      therefore **per image**, not global: the two font atlases and the icon sheet are 4bpp and the front
      end's photographic art is 8bpp. The 256-entry CLUT the section carries per standalone image cannot tell
      them apart — it is fully populated on all of them, `chars.lbm` included.
      Recorded as `q2_vram_ui_images` in `src/formats/vram.[ch]`, together with the complete slot map from
      `0x8003FE20` (thirty images, and slot 4 is reused by four of them, which is why name resolution matters
      here for the same reason it does for texture pages).
      So what remains of split screen in the overlay is what was already known and already implemented: the
      notification layer narrows to 2 / 1 / 1 lines by player count. There is nothing else to find.
      **Three orphan words.** The icon vocabulary carries `was`, `die` and `door`, and no located string uses
      any of them — the shape of a frag-message template (`&P was &O`) that was cut. That is the whole of
      what is left of this entry, and it is an answer: a vocabulary entry with no user is the same finding
      as the Tank Commander's five `tnk_` sounds that appear in no bank (#104) and the two LASERWALLs whose
      object slots are empty (#117). The disc carries the pieces of things that were not finished.

---

## UI conformance against retail capture

Twelve screenshots of the retail PAL game were compared against the reconstruction. This is the first time
any of it has been checked against the running console rather than against the executable alone, and it
moved three things.

**Confirmed by the capture, having been derived only from the code:**

| what | evidence |
| --- | --- |
| the three faces and their letterforms | every screen's text is the reconstruction's `chars.lbm` / `frontend.lbm` faces |
| the full-width gradient selection bar, white text on it | OPTIONS, PLAYER, SOUND, START, SAVE?, CREDITS |
| the slider: white bar when selected, blue when not | SOUND — MUSIC selected and white, SOUND FX blue |
| the toggle: `LABEL ON OFF`, live value bright, other dim | PLAYER — `CROSSHAIR ON OFF` |
| greying takes a row out of the navigation | START's `MULTI PLAYER`, CONTROLS' `SWAP Y OFF` |
| the PLAYER page's items and rows | CROSSHAIR / AUTOCENTRE / CONTROLLER / RESET TO DEFAULTS, exactly the transcribed y values |
| the memory-card SAVE? screen | question, YES on the bar, NO below — the transcribed table, row for row |
| the MISSION screen as a six-row table | `Location` / `Secrets` / `Kills` header on ONE row, columns at +0 / +176 / +256 |
| the 56-pixel centred value fields | `4/4`, `20/20`, `99/100` land where `0x8002260C`'s odd/even rule puts them |
| `control.lbm` is a controller diagram with callouts | the CONTROLS page draws exactly the decoded image |

**Corrected by the capture:**

1. **There IS a status bar** — health, ammo and armour in large numerals beside icons, plus a weapon strip.
   §11.1 is retracted; see #20c. This is the single biggest error the project had.
2. **The page title gets a selection bar too**, not just the cursor row. It follows from the rule already
   read (`+0x48` set, text non-empty) but had been implemented as a special case that excluded the title.
   CREDITS and every OPTIONS-family screen show it.
3. **The `%3X` reading was backwards.** An earlier pass here inferred from the unpadded format that the
   MISSION value columns had to sit at x >= 0x100. They do not — the capture shows the Location column near
   x = 104 — so Sony's `sprintf` is not space-padding and the inference was unsound. The port emits padded
   digits, which reaches the same pen position without depending on the libc.

**What the capture made visible, and what became of it.** Five items were opened here. Three are now closed
and one is half closed; the two that remain both reduce to the same thing — the front end builds its menu
item records at run time instead of transcribing a table, so there is nothing to read out.

- [x] 41. **The UI panel frame — SOLVED.** It is `frontend.lbm` after all, in the strip at rows 223-255 that
      the menu font's tables never reach. `0x8003F0E4` lays down two black semi-transparent tiles (a quarter
      of the scene shows through, because it draws the same tile twice at 50%) and `0x8003E8D0` builds the
      border out of eight `POLY_FT4` cut from one corner, one horizontal edge and one vertical edge -
      mirrored by writing the screen corners backwards against ascending UVs, not by flipping the UVs. It
      overhangs its rectangle by 10 and 6. FORMATS §11.10; `src/menu/panel.[ch]`; `tests/test_panel.c`.

- [~] 42. **The button prompts — the art, table and ordinary-menu policy SOLVED.** The reason a
      string sweep never found `BACK` is that there is no such string: each prompt is one 76 x 16
      pre-rendered cell in `frontend.lbm`, glyph and word together, drawn as a single quad. The table is
      three 18-byte records at `0x8009B4D8` giving uv, size, x and a target/current y pair; the three sit at
      the quarter marks of the 512-pixel screen, and they **slide** three pixels a frame towards the target.
      `q2_menu_open` parks all three below the screen on every page open, so a screen that wants a prompt has
      to ask again. FORMATS §11.11; `src/menu/prompt.[ch]`.
      `0x8001A280..0x8001A348` settles the common policy: SELECT follows a non-NULL action on the current
      row and BACK follows a non-NULL page back handler, at caller y 216 in QFRONT and 220 in-game. QFRONT
      module+`0x459C` adds RULES at y 220 for the first three MULTIPLAYER mode rows and parks it on the two
      settings rows. `q2_prompt_sync_menu` and the client now apply and render those rules. *Still open
      outside the ordinary menu:* the retail drawer's page-11 BACK special case at (230,114).

- [x] 43. **The briefing screen — SOLVED.** `0x800215A0`. It is a sibling of the MISSION screen only in that
      it hands one markup string to the same printf: MISSION positions every run with an explicit `@XXXYY`
      pen escape because it is a table, and the briefing sets margins once and writes flowing text because it
      is prose. Box (96, 32, 336, 100) from `gp+300..306`, inside the §11.10 panel. Its colours are written
      into the text context rather than escaped into the string, and its `#000000` at the end clears the wrap
      flag so the next screen is not still wrapping at 406.
      The three fields come from the map's own `Strings` chunk, which this pass also decoded — see the note
      under #45. FORMATS §11.12; `src/game/briefing.[ch]`; `q2psx-inspect text <map>`.

- [~] 44. **The front end's own menus.** *(#100 reads the pages out of the module and makes the transcription checkable; what remains is driving them.)* A separate page set from the in-game pause menu — the in-game
      OPTIONS at `0x8009AB14` has three items and no credits entry. Capture shows at least:
      the title screen (START / OPTIONS); START -> SINGLE PLAYER / MULTI PLAYER; OPTIONS -> PLAYER / SOUND /
      VIDEO OPTIONS / **VIEW CREDITS**; MULTIPLAYER -> DEATHMATCH / TEAM DEATHMATCH / VERSUS /
      **LOAD SETTINGS / SAVE SETTINGS** (the memory-card entry, #20e); the DEATHMATCH setup with a
      `2 3 4 PLAYERS` choice row, a map name, a **preview thumbnail from `multipics.lbm`**, FRAG LIMIT,
      TIME LIMIT, GAME VARIABLES and PROCEED; a versus player-setup screen with a **blue START bar for
      player 1 and a RED one for player 2**; and CREDITS.
      Three things this confirms rather than adds: `multipics.lbm` really is the deathmatch map previews
      (#39's retraction), the bar colour really is per player with 0 = blue and 1 = red (§10.8's table), and
      the choice widget really does render its live value bright with the alternatives dim.
      The front end reuses the same font, bar, slider, toggle and choice, so **only its tables are missing**
      — everything that draws them is already reconstructed.
      **What this pass added.** The front end is page **46**, entered at `0x80079364`, and it is special-cased
      inside `q2_menu_open` itself (`0x8001A40C`). Its own screens are a **level**: the level table's record 0
      is `QFront` -> `LEVELS/QFRONT/`, which is on the disc and whose `ModelNames` are `Q2LOGO`, `q2title`,
      `q2logowire`, `joypadwire`, `Quaddamage` and the four coloured player models `Male2`, `Male2aqua`,
      `Male2purple`, `Male2red`. So the title screen is a rendered scene with menu text over it, not a page of
      art — which is why no static page table for it exists in the executable. Page 46 installs exactly **two**
      item records (`0x800A3314` and `0x800A3344`, filled at run time).
      A dev-time path `LEVELS\TITLE\` at `0x800AD090` is a leftover: no such directory ships.

      **Correction: those two records are not START and OPTIONS.** This entry read them as matching the
      capture, and the bytes say otherwise. `0x800A3314` holds `{ char *text; s16 x; s16 y; }` = `0x800AECC8`,
      256, 124 — and `0x800AECC8` is the string **`LOADING`**, centred. `0x800A3344` is all zeros, and
      `0x8001A474`'s own `lw v0, 0(s3); beq v0, zero` skips a record whose text pointer is null, so the
      second call installs nothing at all at that moment. What `0x80079364` sets up is therefore the front
      end's **loading screen**, shown while `LEVELS/QFRONT/` streams in — which fits: it is the first thing
      `q2_menu_open`'s special case does, before the level that the title screen is drawn over exists.
      **#135 narrows that: it is not the front end's, it is EVERY level's.** `0x80079364` is the tail of
      `0x80079178`, which the transition opcodes call with the name of whatever is being loaded, and the
      front end arriving is one caller of it among many.
      **And the front end's own code is not in the executable at all — it is `QFRONT`'s `LevelBin`.**
      That is why every sweep for START and OPTIONS failed, and why the item records are "filled at run
      time": the thing filling them is a relocatable module, exactly as `QMULTI.C` is for deathmatch. This
      entry already had the piece that gives it away and did not follow it — record 0 of the level table is
      `QFront` -> `LEVELS/QFRONT/` — and QFRONT ships a **118,216-byte `LevelBin`**, by far the largest
      chunk in it, against 13,008 bytes of `LevelRel`. Its `Strings` chunk is a red herring: three
      placeholders, `sTRING`, `Another String` and a lorem-ipsum wrap test.
      The module's text pool sits at its very front, before the code, and it is the whole front end —
      `q2psx-inspect modstrings QFRONT` lists 349 runs and the first eighty are the menus:

          START / OPTIONS · SINGLE PLAYER / MULTI PLAYER · NEW GAME / LOAD GAME
          EASY / MEDIUM / HARD · PLAYER OPTIONS / SOUND OPTIONS / VIDEO OPTIONS / VIEW CREDITS
          CROSSHAIR · AUTOCENTRE · CONTROLLER · RESET TO DEFAULTS
          "      MUSIC" / "   SOUND FX" · STEREO · HORIZONTAL SPLIT · SCREEN POSITION
          DEATHMATCH / TEAM DEATHMATCH / VERSUS · LOAD SETTINGS / SAVE SETTINGS
          "2 3 4 PLAYERS" · "TIME LIMIT   10" · "FRAG LIMIT   10" · GAME VARIABLES
          GRAVITY · FALLING DAMAGE · WEAPON STAY · ONE SHOT KILL · GAME SPEED · BLAST FORCE
          INFINITE AMMO · ALL WEAPONS · RULES, and the five modes' rule paragraphs in full

      Two things fall out of the list that the capture could not show. The **leading spaces are the layout** —
      `"      MUSIC"`, `"   SOUND FX"`, `"    GRAVITY"`, `" GAME SPEED"` are padded to right-align against
      their sliders, which is why the in-game SOUND page's transcription needed no such padding and the
      front end's does. And there are **five** deathmatch rule paragraphs, not three: alongside the three
      selectable modes the capture shows, the pool carries full rules for a flag-capture mode and a
      last-one-standing mode, matching the six modes `QMULTI.C` implements of which three are selectable
      (#0). BRONZE cheats have an unlock file, and there is a `DEMO OF GAME` / `STARTING` pair for the
      attract loop.
      `q2psx-inspect modstrings <map> [crea]` is the reader; BASE0's `LevelBin` yields five runs against
      QFRONT's 349, which is the contrast that says the scan is finding a pool rather than manufacturing one.
      **And the item records are not built at run time either — they are a static array in the module,**
      which `modxrefs` finds by asking what points at a string. Every one of the eight checked has exactly
      one word reference, and it is the record's own first field. The layout is the executable's own, 24
      bytes: `{ char *text; s16 x; s16 y; void (*action)(void); ... }`, so the engine's `0x8001A474` takes a
      module's record and the executable's without knowing the difference.

          module+0x0EC3C  START            256, 151  -> module+0xCDC4
          module+0x0EC54  OPTIONS          256, 177  -> module+0xCCA4
          module+0x0EC84  SINGLE PLAYER    256, 111  -> module+0xCD40
          module+0x0EC9C  MULTI PLAYER     256, 137  -> module+0xCF68
          module+0x0ED44  PLAYER OPTIONS   256,  85  -> module+0xCADC
          module+0x0ED5C  SOUND OPTIONS    256, 111  -> module+0xCB74
          module+0x0ED74  VIDEO OPTIONS    256, 137  -> module+0xCC0C
          module+0x0ED8C  VIEW CREDITS     256, 163  -> module+0x35C8

      Two things that are layout rather than data. Every row is **centred at x = 256** and the pitch is
      **26 pixels** — 85, 111, 137, 163 on the OPTIONS page, 111 and 137 on START's, 151 and 177 on the
      title's. And the title page's two rows sit *lower* than any sub-page's first row, which is the space
      the `Q2LOGO` model occupies above them: the title screen is a rendered scene with two lines of menu
      over it, as this entry said, and now the two lines have coordinates.
      **Three more pages came out of the same walk**, and every string in them has exactly one word
      reference, so the arrays are contiguous 24-byte records like the first three:

          module+0x0EF9C  NEW GAME        256, 111  -> module+0xD0AC
          module+0x0EFB4  LOAD GAME       256, 137  -> module+0xD400
          module+0x0EFE4  EASY            256,  98  -> module+0xD380
          module+0x0EFFC  MEDIUM          256, 124  -> module+0xD3A8
          module+0x0F014  HARD            256, 150  -> module+0xD3D4
          module+0x0F104  DEATHMATCH      256,  80  -> module+0x4AD8
          module+0x0F11C  TEAM DEATHMATCH 256, 102  -> module+0x4AD8
          module+0x0F134  VERSUS          256, 124  -> module+0x4AD8
          module+0x0F14C  LOAD SETTINGS   256, 146  -> module+0xD148
          module+0x0F164  SAVE SETTINGS   256, 168  -> module+0xD1FC

      Two things in there are not guessable from the capture. **The five-row page is tightened to a
      22-pixel pitch** — 80, 102, 124, 146, 168 — where every two-, three- and four-row page in the front
      end is 26. And **the three deathmatch modes share one action**, `module+0x4AD8`, so the mode is
      decided by which row is on rather than by three handlers, which is what `QMULTI.C` wants: it
      implements six modes of which three are selectable (#0).
      The flow they describe is START -> SINGLE PLAYER -> NEW GAME -> a difficulty, and only the difficulty
      begins the game — the port follows it, and hands the chosen skill to `q2_cre_set_skill` before the
      level loads so the creatures that load already have it.
      **The deathmatch SETUP page is the last one the capture shows, and its rows are not widgets** — this
      entry guessed they were and that was wrong. `module+0x0F914` is six of exactly the same bare 24-byte
      records, with bytes +8 onward all zero: no widget field, no setting index, no bound variable.

          module+0x0F914  "2 3 4 PLAYERS"    256,  64   no action
          module+0x0F92C  (filled at run time) 256, 81  no action
          module+0x0F944  "TIME LIMIT   10"  256, 155   no action
          module+0x0F95C  "FRAG LIMIT   10"  256, 172   no action
          module+0x0F974  "GAME VARIABLES"   256, 189  -> module+0x49F8
          module+0x0F98C  "PROCEED"          256, 206  -> module+0x59CC

      **The values are in the TEXT.** The pool holds `"TIME LIMIT   10"` and `"FRAG LIMIT   10"` with the
      number padded into the string, and the module rewrites it in place — the module image is RAM. That is
      the same device as the leading spaces on `"      MUSIC"` and `"    GRAVITY"`, which right-align against
      their sliders: this front end lays out with padding rather than with fields, which is why none of its
      records needs a widget.
      The row at y = 81 carries a short string filled at run time — the chosen map's name — and the
      74-pixel gap below it is where the capture's `multipics.lbm` preview goes.
      **The padding and the x coordinate are the same fact.** Following the second word reference that
      `2 3 4 PLAYERS` and `GAME VARIABLES` each carry reaches the VERSUS setup at `module+0x0F9BC` and the
      GAME VARIABLES arrays at `module+0x0F194`, and the variables rows are the first in this whole front
      end that are **not centred at x = 256**:

          module+0x0F194  "    GRAVITY"        x 168, y  97
          module+0x0F1AC  "FALLING DAMAGE"     x 256, y 124
          module+0x0F1C4  "RESET TO DEFAULTS"  x 256, y 151

      The rows at 168 are exactly the ones whose strings are PADDED — `"    GRAVITY"`, `" GAME SPEED"`, and
      on the SOUND page `"      MUSIC"` and `"   SOUND FX"` — and the rows at 256 are exactly the ones that
      are not. So the padding is not decoration: a padded label is left-anchored at 168 so its text ends
      where the slider begins, and an unpadded one is centred at 256 because it has no slider to meet.
      That is one rule covering both observations, and it is checkable on every row in the pool.
      There are also **several variables arrays**, not one — `"    GRAVITY"` appears at `+0x0F194` (y 97)
      and again at `+0x0F1F4` (y 76) with different neighbours — which is the same shape as the in-game
      VARIABLES page's four cheat-level variants at `0x8009A6C4` (§10). Eight toggles spread across them.
      VERSUS at `module+0x0F9BC` is four rows: the player count at y 63, the run-time map name at 82,
      a third string at 153 and GAME VARIABLES at 172.
      *Still open:* which variables array goes with which cheat level, and what each `action` does beyond
      the page it opens.

      *Also still open: the SCENE the title is drawn over,* and the reason is worth stating so nobody
      invents it. QFRONT's world is two nodes and eight vertices — there is no room in it for a title
      screen. The picture is its `ModelNames`: `Q2LOGO`, `q2title`, `q2logowire`, `joypadwire`,
      `Quaddamage` and the four coloured player models. Every one of them is authored **centred on its own
      origin** — `Q2LOGO`'s posed bounds are `[-725 -916 -80] .. [727 1081 81]` — and QFRONT's single
      `StartPos` puts the eye at the world origin, so drawing them where they sit would put the camera
      inside the logo. Their placement is the module's, and until it is read there is no honest position to
      draw them at. The port therefore shows the menu over an empty scene rather than a guessed one.

      **Answered: the scene is five ITEMS, and the spinning logo spins because it is one.** The reader that
      settles it is new — `q2psx-inspect modbytes <disc> <map> <addr> [n]` hex dumps a *relocated* module's
      image, which nothing could do before: `bytes` reads the executable and cannot see a module at all, so
      every question about a module's DATA had to be answered by disassembling whatever walked it. This is
      the first one that needed the table itself.
      `init` calls `module+0xC5BC(module+0x10D7C, module+0x12B20)` and that function is a five-line spawner:
      it walks a -1-terminated list of **item table ids** — `57 Q2LOGO`, `59 Male2`, `60 Male2red`,
      `61 Male2purple`, `62 Male2aqua` — pushing each through a 16-byte `q2_pop_place` template at
      `module+0xE48` into `engine+0x2C`, which is `0x800599DC`, **the engine's own item spawner**. Then it
      overwrites the entity by hand:

          8010C658  sh  2048, 232(v0)   +0xE8  yaw, half a circle
          8010C660  sw  zero, 268(v0)   +0x10C render_flags
          8010C664  sw  zero, 164(v0)   +0xA4  origin[0]
          8010C668  sw  zero, 168(v0)   +0xA8  origin[1]
          8010C66C  sw  1700, 172(v0)   +0xAC  origin[2]
          8010C680  jalr v1             engine+0x114 = 0x80089E38, RotMatrix

      So every object stands at `(0, 0, 1700)` facing yaw 2048, and **the eye is not inside the logo after
      all** — it is 1700 units in front of it, which is what this entry could not know without the table.
      All five records carry the `spin` flag, and ids 57..64 are exactly the eight `records no map ever
      places` that `q2psx-inspect items` has always listed. That is what they are for.

      **Only one of the five is ever shown, and it does not use the item think.** `module+0x3414` — the two
      lines every front-end page builder opens with — hides all five (bit 0x80 of `+0x118 + p*100`, all four
      players) and its tail shows exactly one back:

          80103554  lh   v1, 710(engine)   ; the live page id
          8010355C  beq  v1, 11            ; page 11 keeps even this one hidden
          80103574  and  v0, ~0x80         ; objs[0] shown
          8010358C  bne  v0, zero          ; page 0 is the title screen
          801035B0  sw   v1, 60(objs[0])   ; +0x3C — the THINK, per page

      The four coloured player models are spawned and never drawn on any page reached so far, and the logo's
      motion is not `0x80059330` at all — the module overwrites `+0x3C` with one of its own on every page
      change: `module+0x9D24` on the title screen and `module+0x9E0C` everywhere else. They differ in one
      thing each:

          module+0x9D24   scale += 256, capped at 4096      module+0x9E0C   scale -= 256, floored at 1024
          both            yaw   -= 4 * dt

      So the logo GROWS into the title screen from nothing and SHRINKS to a quarter when you step into a
      sub-page — an animation no still capture could show — and it turns at `4 * dt` against the item spin's
      `3 * dt` (`0x8005947C`), so a port that reused the item think would run it a third slow. `dt` is
      `*(engine+0xD4)` = `0x800B2DB4`, the same level-clock delta the item think reads.
      **And the marking is not a draw flag.** The only reader of bit 0x80 is the deathmatch respawn
      countdown at `0x80059374`; the front end has no players for the touch sweep to walk anyway. It is the
      module keeping the sweep off its own scenery.
      The port spawns all five, hides four, runs the two thinks and steps the entity set while the front end
      is up — which it did not do before, because "the world is frozen while a menu is open" is right for a
      pause menu and wrong for a title screen that IS a running level. `q2psx-inspect menu pages "" QFRONT`
      prints the list with each id's table record; `src/game/levelbin.[ch]`, `q2_sim_attach_scene`.

      **The title screen's LIGHTS are the module's too, and they are the reason it renders dark without
      them.** `q2psx-inspect lit QFRONT` accepts zero lights at the spawn point and the map's `Lights` chunk
      is 28 bytes, so shading the logo through the light path takes it to black. That is not a gap in the
      map: the front end lights its own scene, from code, every frame.
      The menu's own frame at `0x8001A1E8`/`0x8001A200` calls two module hooks — `engine+0x298` and
      `engine+0x294`, which `init` fills with `module+0x32BC` and `module+0x2BD8`. The first is only the
      controller-unplugged check (page 20, the `PLEASE INSERT / CONTROLLER INTO / CONTROLLER PORT XX`
      records at `module+0x11670`). **The second is a five-light rig**, reaching `engine+0x3C` —
      `0x80075C34`, *append a runtime light* — five times a frame.
      The signature had to be read off the CALLEE, because the call sites write their arguments to
      `sp+48..56` and that is not where an o32 stack argument goes. `0x80075C34` spills a1/a2/a3 into the
      caller's own home slots and reads them back as halfwords, so there are only ever four register
      arguments — `a0` a `s32 *pos`, `a1` the packed rgb, `a2` inner-low/outer-high, `a3` style/size — and
      `sp+48` is the local `pos[3]` whose address `a0` carries (`addiu s3, sp, 48` in the prologue). With
      that the rig reads out whole:

          three at (0, y, 500) for y = -200, 0, +200    rgb (16,255,64)   100/200
          two   at (x, 600, 900), x mirrored            rgb (64, g, 127)  500/1500

      The two big ones are alive and the three small ones are not. Each big light draws `rand()` twice a
      frame: its GREEN channel is `(rand() & 63) + 64`, so it flickers between 64 and 127 while red and blue
      hold, and its X eases a quarter of the way toward `±((rand() & 511) + 200)` every frame — one right,
      one left, from persistent stores at `module+0x11664` and `+0x11668` that the module image starts at
      zero. That drift is what makes the retail title screen's lighting move while the geometry only turns.
      `rand()` is BIOS `A(2Fh)`: `engine+0x10C` is `0x80089E28`, the two-instruction thunk
      `addiu t2,0xA0; jr t2; addiu t1,47` — the generator `q2_rng_next` already reproduces bit for bit. The
      four draws happen in a fixed order and it is kept, because they share one generator and reordering
      them changes every value after the first frame.
      **The three small ones never reach the logo**, and that is not an error to tidy: 200 of outer radius
      against 1200 of separation, and `q2_light_set_add` rejects on any axis delta at or beyond the radius.
      The rig places them and the gather throws them out. They are transcribed as they are.
      One consequence is load-bearing: the dynamic list holds sixteen and `0x80075C34` drops the seventeenth
      silently, so the list must be cleared per frame or the rig fills it in four frames and the logo is
      then lit by a frozen snapshot of frame four.

      **Three smaller corrections this pass owes.**
      *The camera is the world origin, not the spawn point.* `engine+0x170` — which `init` calls with 0
      before anything else — is `0x80077D0C`, and its first act is `memset(0x800D5C30, 0, 3920)`: the whole
      five-viewport array zeroed, position and rotation included, before `engine+0x174(0, 160, 4000)` puts
      the projection and far plane back. So the front end throws QFRONT's `StartPos` away deliberately. It
      also writes **1** into the player count at `0x800B2C2C`, which is why the module bothers to mark its
      scenery taken: with one player registered the touch sweep really would run over it.
      *`proj = 160` is confirmed from a third direction* — `engine+0x174(0, 160, 4000)` agrees with
      `SetGeomScreen`'s eleven call sites and with the sky-wedge measurement.
      *The logo is NOT born dark.* An earlier pass here had its intensity grow from zero on the first frame, and
      nothing in the module zeroes `+0xFC`: `q2_item_spawn` leaves a non-materialising record at full size
      and the think reads what is there. What the title screen's ramp is for is the way BACK — a sub-page
      has driven the intensity to 1024 and returning walks it up to 4096 again. It is one fade seen from
      two ends, and it only runs after you have been somewhere.
      And the earlier guess in this entry that the rows sit low because the logo occupies the space *above*
      them is superseded by the geometry: at scale 4096, z 1700 and proj 160 the posed bounds
      `[-916 .. 1081]` in y project to screen y 38..226, so the logo fills the frame and the two rows sit
      over its lower half. The rows are low because that is where the model's legs are, not because the
      model stops above them.

      **And the logo settles a question about `0x800B2CC8` that had nothing to do with the front end.**
      `screen.h` called that field "the distance past which an entity is not drawn at all". It is `far / 4`,
      the front end installs `far = 4000`, and the one model it draws stands at z = 1700 — so a cut-off read
      that way would blank the retail title screen. Whatever the field gates, it is not a plain compare
      against an entity's z. The note is corrected and nothing culls on it.
      **And the background is BLACK, which is the one thing about this screen a capture settles instantly.**
      The client painted it navy — it wrote `(16, 16, 32)` into the display's clear colour every frame and
      cited `0x800780C0` for it. That function writes no colour: it zeroes `view+260` on every viewport and
      calls the full-screen background env. The colour is gp+1604, and the whole image holds exactly three
      references to it — `0x80076A00` reads it into the env's rgb, and `0x8006E0B0` and `0x80070FA0` each
      `memset` it to zero. Four zero bytes on disc, no writer that puts anything else there. The invented
      navy was invisible in a lit corridor and obvious the moment the front end drew one model over it.
      One more thing the entity draw owed: **the light intensity is the entity's own `+0xFC`.** `0x8006B298`
      folds `+0xFC` and `+0xFE` as `(a * b) >> 11`, the allocator sets both to 4096 at `0x8006C1B8`, and
      `+0xFC` is an intensity — so an item is lit in proportion to its fade. The port passed a
      neutral pair, which agrees with the engine only for entities at full size. It shows here because the
      logo's own think moves that field: dimmed on a sub-page it is lit at a quarter.

      *Read in full by #126:* the **attract loop**. `module+0xCEE0` parks 9000 (30 s at 1/300) in
      `module+0x12DC0` and installs `module+0xC6AC` as the page hook; any input resets it and zero calls
      `0x80101B08`, which puts up `DEMO OF GAME` and plays no film. The `STARTING` half of that string pair
      belongs to the OTHER countdown — the one the difficulty menu arms, which is the opening reel.

      **The thread to pull is the module's engine vtable**, and it is worth writing down because every
      `LevelBin` reaches the engine the same way — `QMULTI.C` included. A module holds the block at its own
      `+0x8`, the loader writes it, and the installer that fills it is the long run of stores from
      `0x80079818`. So a slot is named by grepping that function for its offset:

          +0x1C   0x8003B250      +0x20   0x8007F328
          +0x170  0x80077D0C      +0x174  0x800781F0

      QFRONT's `init` (export 0, module+0x30F4) calls `+0x170` with 0 and then `+0x174` with
      **(0, 160, 4000)** before it does anything else. 160 is the world's own projection distance and 4000
      a far plane, so that pair is the front end setting up **its own viewport** — which is an independent
      corroboration of `proj = 160` from a code path that has nothing to do with `SetGeomScreen`'s eleven
      call sites or the sky-wedge measurement against the retail capture.
      Following `init` past that produced three more things, and one of them is behaviour rather than data.

      **The front end dispatches on a mode the engine hands it.** `init` reads `engine+0x4A8` and branches:
      1 goes to `module+0x35C8`, 2 to `module+0xCF68`, anything else to `module+0xCEE0`. Those first two are
      the actions on the VIEW CREDITS and MULTI PLAYER item records, so the word is a **re-entry mode** —
      how the front end comes back up on the right screen after a game or a deathmatch ends rather than
      always at the title. `module+0xCEE0` is the title page's own builder.

      **The title screen starts an attract demo after thirty seconds.** `module+0xCEE0` parks 9000 in
      `module+0x12DC0` and installs `module+0xC6AC` as the page's per-frame hook, and that hook is a
      countdown: any input resets it to 9000, otherwise it subtracts the frame delta and calls
      `0x80101B08` when it reaches zero. 9000 of the console's 1/300 s units is **30 s**, and it is what
      the `DEMO OF GAME` / `STARTING` pair in the string pool is for.

      **The models are shown and hidden, not placed per page.** Every builder starts with
      `module+0x3414(list, mode)`, which calls `engine+0x1E4(mode)` and then walks a table at
      `module+0x12B20` — built at `init` from `module+0x1163C` — setting bit `0x80` in three fields (`+0x118`,
      `+0x17C`, `+0x1E0`) of each object it names. So the scene's models are placed once and each page turns
      the right ones on, which means the logo's position is in that object table rather than in per-page
      code. Two more engine slots come with it: `+0x200` installs an item record array (the title's call is
      `(module+0xEC3C, 32)`, against the executable's own `0x8001A474(record, 16)`) and `+0x290` is the
      per-frame page hook.
      **`module+0x1163C` is four 8-byte records**, terminated by a zero word:

          +0x1163C  data 0x80110D94   a -1   b 0
          +0x11644  data 0x80110F94   a -1   b 3
          +0x1164C  data 0x80111194   a -2   b 1
          +0x11654  data 0x80111394   a -2   b 2

      The `data` blocks are 0x200 bytes apart, so each object owns 512 bytes. `b` is 0, 3, 1, 2 — an index
      the builder passes on as its own argument — and `a` is -1 for the first pair and -2 for the second,
      which is what makes them two pairs rather than four singles.
      The builder at `module+0x22C4` walks the list, and for each entry hands `engine+0x184` a four-halfword
      record built on the stack as `{ 0, 255 - i, 256, 1 }` together with the object's data pointer. The
      descending `255 - i` is a draw priority and 256 is a unit scale in the port's usual 1.8.8.
      *What is left for the scene:* the 512-byte object blocks themselves, and `engine+0x184`, which
      consumes them. **Two dead ends recorded so they are not walked twice.**
      `engine+0x184` cannot be named the way the other slots were: the installer's store is
      `sw a1, 388(v1)` at `0x80079E3C`, so that slot is filled from an installer ARGUMENT rather than a
      constant, and grepping the installer for the offset gives the parameter, not the callee. It has to be
      reached from the installer's own caller.
      And the object blocks are not coordinates. `0x80110D94` is 512 bytes of dense data with no run of
      small signed values anywhere in it — nothing that reads as a position triple in any of the port's
      fixed-point formats. Whatever a position is here, it is not stored plainly in the block, so the block
      has to be identified before it can be read.

- [x] 45. **Word wrap in practice — SOLVED, and it was the wrong screen.** The wrap the capture shows is the
      **briefing's**, not the MISSION screen's: `#06A196` at `0x800AE740` sets margins 106 and 406, which is
      the only place in the game that turns the flag on, and `#000000` at `0x800AE758` clears it again. The
      MISSION screen genuinely does not set margins — it is a table and positions every field itself. The
      port's `#` escape was already correct; what was missing was a caller.
      Along the way the **`Strings` chunk** turned out to be the text database the wrap operates on. It had
      been catalogued (FORMATS §2.8) but never decoded: it is a `{char key[12]; u32 offset}` dictionary read
      by the name lookup at `0x800701B4`, 49/49 decode, 360 entries, 34 maps with real text. `MapTitle` is
      the briefing's `Location:`, `Unit<N>Miss1` its `Mission Objective:` and `Unit<N>Curr<S>` its `Current
      Orders:` — and the step is **hexadecimal**, because SECURITY runs `Unit2Curr1`..`Unit2CurrA`.
      `src/formats/leveltext.[ch]`; `tests/test_leveltext.c`; `q2psx-inspect text`.

---

## In-game conformance against retail capture

The UI section above compared twelve menu screenshots. This is the first comparison against a frame of the
game being **played** — the BASE0 spawn, looking down the opening canyon, blaster in hand — and it settles
two things that the executable alone could not, and opens one.

**The picture is 4:3.** The capture is pillarboxed inside a 16:9 frame with the game filling a 4:3
rectangle, so the 512 × 248 buffer is presented at 1.333:1 and a framebuffer pixel is about 0.646 as wide as
it is tall. Nothing in the executable says this — it is what the display does with what the GPU emits — and
the port had been showing the buffer one pixel per window pixel, which is a **1.5× horizontal stretch**.
`Q2_SCREEN_FIT_FULL_4_3` is now the default and the client letterboxes or pillarboxes into any window;
`Q2_SCREEN_FIT_TELEVISION` keeps the stricter hardware reading (all five horizontal modes span the same
active line, PAL fills the 4:3 raster with 256 of them, so a pixel is exactly 2:3 and the 248 drawn lines
come out at 1.376:1). The two differ by 3%.

Two independent pieces of the game's own art agree with a pixel narrower than it is tall, which is worth
recording because it is evidence internal to the disc: the `qk_menu.lbm` menu faces and the 8 × 8 `chars.lbm`
HUD face are both drawn texel-for-pixel into square rectangles and are both authored about 1.35× wider than
tall — a normal letterform only once the display has narrowed the pixels.

**The world's field of view is right.** With `proj` 160 over the 512 × 248 viewport the reconstruction puts
the canyon's sky wedge at 0.37–0.53 of the picture width against the capture's 0.33–0.47 — the same 0.16
width, offset by a slightly different yaw. So the one-player layout's 116.0° × 75.6° is confirmed against the
running game, and with it the reading that `view+262` is `SetGeomScreen`'s argument.

That also means **the console's own picture is anamorphic**, by exactly 1.5. The GTE has one projection
distance and it reaches SX and SY alike, so the frustum is symmetric in framebuffer pixels while the display
is 1.333:1. There is no term anywhere that undoes it: the only matrix scale on the world's transform chain is
the uniform `(768, 768, 768)` at `0x800AEB30`, applied per column by `0x80055AF8` — an object scale of
exactly 3 — and the view weapon's own chain (`RotMatrix` at `0x8004F464`, `MulMatrix` at `0x8004F474`) has no
scale call at all. The squeeze is the game's, not the reconstruction's, and must not be "corrected".

- [x] 46. **SOLVED — and it WAS the projection. The port's horizontal field of view was one and a half
      times too wide, for the whole renderer.**
      `0x80055DE4` builds the matrix the world draw loads from view+160 (`0x800313DC`) by scaling a
      basis ROW BY ROW — row 0 by `vw/320`, row 1 by `vh/240` — with `vw` the immediate 320 at
      `0x800779BC` and `vh` the immediate 160 at `0x80077948`. The console's camera is
      `diag(1, 2/3, 1)` and its effective projection `(H, 2H/3)`; the port had `diag(1,1,1)` and
      `H = 160`, rendering `(160, 160)`.

      **What made it provable was a capture at the spawn, before the player moved**, at native
      resolution. The 2D briefing panel is at x 86..441, y 26..137 in BOTH frames, which pins a shared
      framebuffer origin and width and makes every other number a direct comparison:

          sky wedge above the panel   console 136 px    before  90 px     after 135 px
          weapon emitter x            console 325..337  before 301..307   after 322..333
          weapon emitter y            console 157..166  before 157..166   after 157..167

      Vertical was already exact and stays exact; the horizontal closes from 27 pixels to 3, and the
      WORLD closes with it — which is what makes this the projection rather than the weapon. Every
      earlier measurement in this entry was of a symptom.

      Carried by `q2_rotation_view_anamorphic`. Which half holds the factor is ambiguous and is stated
      there: the console reaches (240, 160) as `H = 240` with row 1 at two thirds, the port reaches it
      as `H = 160` with row 0 at three halves, and the pictures are identical. The second form is used
      because it leaves every layout's VERTICAL alone, and only the full-screen one has been measured.
      `0x800781F0` would settle it by naming the distance but is reached only as a function pointer
      (`0x80079CB4`), so a relocated module can set it.

      *The original entry, and the thirteen things it eliminated, follow.*

- [x] 46a. **The original entry: the view weapon sits too far left, and it is not the projection.** — *the pitched case is
      solved by #96; what is left is the still frame at rest.* Placed through
      `q2_vw_place` at the BASE0 spawn, the blaster's drawn geometry spans x 278…376 of the 512-wide
      viewport — 0.54…0.73 across. The capture has it running from about 0.79 to off the right edge, with
      the forearm entering from the bottom-right corner rather than from the bottom centre. Vertical
      placement and apparent size are close (the port's y 143…258 against a capture top edge at ≈0.62 of
      the picture height); it is the horizontal offset that is wrong, by roughly a quarter of the screen.

      What has been ruled out. The **projection** is shared with the world, which the sky-wedge measurement
      above confirms is right, and the world and the weapon go through the same `SetGeomScreen` /
      `SetGeomOffset` pair — the eleven `SetGeomScreen` call sites are all viewport or bring-up code, none
      of them in the weapon's `0x8004Fxxx` range, and the only inline writes to `OFX`/`OFY` are at
      `0x80065C54` and `0x80065E0C`, in the world renderer's own displaced-centre path and its restore.
      A **scale** is ruled out by the matrix chain above. The `286 - viewOffset` eye base is confirmed at
      `0x8004F608` and the drop with a crouch measures 290 as it should.

      What is left is the translation itself: `q2_vw_place` puts the grip 140 right, 160 down and 44 forward
      of the eye in view space at rest, and something about that triple — which key the idle clip rests on,
      how `cur_t` interpolates into it, or whether a per-part offset is being dropped — is short by about
      the same quarter screen. `0x8004F5E0`'s operands are the thing to read next.

      **`0x8004F5E0`'s operands have now been read, and they say the port is right.** Three things are ruled
      out, and they were the three this entry pointed at.
      *The translation is the disc's.* `0x8004F47C…0x8004F5DC` interpolates the key's `+6` triple as
      `base + (key - base) * (total - left) / total` and hands it to `0x8006FD38`, which is `(M · v) >> 12`
      with the shift at `0x8006FE08`. The blaster's raise keys read `t 140 257 44`, `140 157 44`,
      `140 157 104`, `140 157 44` — `t.x` is **140 on every key**, so there is no key the machine can rest
      on that puts the grip further right.
      *The interpolation is not it either*, since every key agrees on x.
      *And the rotation ORDER is not it.* `RotMatrix` at `0x80089E38` reads a packed `{sin, cos}` table at
      `0x800A5430` and writes `m[1][2] = -sin(x)` (`0x80089F0C`) and `m[0][2] = sin(y)·cos(x)`
      (`0x80089F20`). Those two elements are the signature of **Ry·Rx** with Z outermost — `Rx·Ry` would give
      `m[0][2] = sin(y)` and `m[1][2] = -sin(x)·cos(y)` — which is exactly what `q2_rotation_euler`
      implements. A wrong order would have mattered a great deal here, because the blaster's clip rotation is
      `(2248, 1280, 1748)`, about 198°, 112° and 154°.
      So `q2_vw_place` agrees with the executable on every operand that can be checked against it, and the
      quarter screen is not in it. What has NOT been checked is the port's own invention: `q2_vw_build_ot`
      cancels the camera by drawing the model with `camera^T · clip`, which is not a transcription of
      anything — the console composes `MulMatrix(RotMatrix(view), entity)` and lets the world draw apply the
      camera. That cancellation is exact only when the two matrices are built the same way, and they are
      not: `q2_vw_place` drops `ang[2]` where the camera's own `q2_rotation_view` includes roll. That is a
      real divergence whenever the strafe lean is active, though it cannot explain a still frame at zero
      roll — so the measurement itself may also want re-taking against a fresh capture.

      **The roll divergence is closed and two more operands have been read; the measurement still wants
      re-taking.** `q2_vw_place` now builds the placement matrix with the camera's own
      `q2_rotation_view(yaw, -pitch, roll)` and applies its transpose, so `view * R_place == I` holds by
      construction and the roll is in it — `tests/test_viewweapon.c::test_camera_undoes_the_placement`
      pins that. Two further things were read out of `0x8004EE0C` and neither is the quarter screen:

      *The block that looks like a per-model offset is DEAD.* `0x8004F644…0x8004F694` calls `0x8006FC1C`
      (a 3x3 matrix times an SVECTOR, result in `a2`) with the composed matrix, `sp+24`, and `sp+32`, then
      adds `sp+32/34/36` to the weapon's world position. The only write to `sp+24` in the whole function is
      the `memset(sp+24, 0, 6)` at `0x8004EE18` — `a0=sp+24, a1=0, a2=6`, through the A0(0x2B) thunk at
      `0x80089E18`. It is a zero rotated by a matrix, added to a position. Nothing to reproduce, and worth
      recording so it is not "found" again.

      *One operand looked missing and is provably zero.* `0x8004F3E4…0x8004F3F8` adds `[combat+70]` to the
      X component of the interpolated clip rotation immediately before `RotMatrix`. `combat` is
      `[entity+12]`, the same block the weapon id (`+98`), the selection (`+102`) and the switch countdown
      (`+222`) live in. **That halfword is read here and nowhere else in the image, and nothing writes
      it**: every `sb`/`sh`/`sw` with an immediate of 68..72 on any base is either a different struct (the
      `0x8001xxxx` sprite block, whose `+64/+66/+70/+72` are its own), a stack slot, a materialised global
      (`0x8003B424` writes `0x800C7CA6`), or library code above `0x80086000`. So the term is `x + 0`, and
      the port's expression IS the console's expression. Not a gap — an operand that costs nothing, named
      here so nobody re-derives it.

      *And the weapon was drawn from RAW VERTICES.* `q2_vw_build_ot` passed `pose = NULL`, so the one model
      in the frame that never got its rest transform was the one in the player's hands. That is now fixed
      and it MOVES the weapon: `Railgun G` shifts by its rest translation of −34 on Z, `Bfg G` by 53 on Y,
      and `HandGren G` goes from a 105-unit lump to a 666-unit arm.

      **THE HARNESS THIS ENTRY WAS MEASURED WITH HAD THE CAMERA 286 UNITS OFF THE EYE.**
      `cmd_viewweapon.c`'s `render_view` set `cam.pos.y = feet + 286 - viewOffset`, which is the console's
      expression written against the ENTITY ORIGIN (`entity+0x58`, 286 above the feet — `0x80038630`).
      Handed the FEET, the 286 has nothing to cancel against, so the camera sat 286 above the eye
      `q2_vw_place` hangs the weapon off and the tool drew the weapon from a viewpoint the weapon was not
      attached to — at one point out of the top of the frame. `q2_sim_eye` computes `feet - viewOffset`
      and says why; this did not. Fixed, and the tool now prints the box as FRACTIONS as well as pixels,
      because a capture is 640 x 480 or 512 x 240 and pixels compare to nothing.

      **Re-measured against a real retail capture of the BASE0 opening.** The port, same map and spawn,
      weapon box against a world-only render of the identical camera:

          port    x 0.543..0.730   y 0.581..1.000
          retail  x 0.550..0.820   y 0.579..1.000   (read off the capture by eye)

      That is a different defect from the one this entry describes. **The left edge and the top edge
      agree**; what was short is the RIGHT edge. "Sits too far left" is withdrawn.

      **And the short right edge was the near plane.** `q2_model_build_ot` marked a vertex invalid when
      `gte_divide` raised `GTE_FLAG_DIV_OVERFLOW` and dropped every face touching it. That flag is raised
      when the projection distance reaches twice the depth — at `h` 160, anything nearer than 80 — so it
      fires on exactly the geometry closest to the eye, which on a view weapon is the grip and the
      forearm. The hardware CLAMPS the quotient to `0x1FFFF` instead of trapping, and whether the game
      then discards the face is a question about the game: **the executable contains no `cfc2 rX, $31`.**
      Twelve `cfc2` in the whole image, not one reading FLAG, so nothing can branch on an overflow it
      never loads. The original draws these faces stretched to the saturation limits. Fixed, and the
      weapon's box widened from 0.188 of the picture to 0.225.

      **Measured across the idle cycle, the port now brackets the capture.** The blaster's idle keeps its
      translation at (140, 157..165, 44) on all fourteen keys and moves the ROTATION instead, by about
      twelve degrees, so the drawn extent depends on the phase — which is why a single sample was never
      going to settle this. `viewweapon` takes a hold in ticks now; sweeping it over the 1,230-tick loop:

          port    left 0.521..0.543   right 0.693..0.803   width 0.162..0.281
          retail  left 0.550          right 0.820          width 0.270

      Retail's width sits inside the port's range and its right edge is a hair above the port's maximum,
      which is inside the error of a number read off a screenshot by eye.

      **And the chain the screenshot was standing in for is now checked directly, which is stronger than
      the screenshot ever was.** Where the weapon lands is the product of the angle sum (`0x8004F40C`),
      `RotMatrix` (`0x8004F464`), `MulMatrix` (`0x8004F474`), `ApplyMatrix` (`0x8004F5E0`), the eye base
      (`0x8004F608`), the GTE's projection registers, and the rule for a vertex that comes too close.
      Every link is now either transcribed from an instruction or re-derived from the image on every run
      of `q2psx-inspect viewweapon`:

      - **the sine table.** `RotMatrix` reads a packed `{sin, cos}` pair per angle at `0x800A5430`
        (`0x80089E60`). `src/common/trig.c` GENERATES its table from `sin()` with half-away-from-zero
        rounding — a different construction entirely — and it agrees with the disc on **all 4,096 entries
        of both halves**, checked every run. One LSB out anywhere would have skewed every rotation in the
        game.
      - **the near-plane rule.** `cfc2 $31` is the only way to read the GTE's FLAG, and there is not one
        in the image: 12 `cfc2` total, none of them FLAG. Checked by sweeping the segment.
      - **the projection centre.** `OFX`/`OFY` are written in exactly four pairs and `H` twice; the world
        renderer's displaced centre at `0x80065C54` is put back at `0x80065E0C`, so the weapon —
        transformed after the world — projects about the same point. "The projection is shared with the
        world" is now a swept fact rather than an assumption.

      **MEASURED PROPERLY AGAINST TWO CAPTURES, AND THE WEAPON IS GENUINELY IN THE WRONG PLACE.**
      `ref/` is gitignored, so the comparison is a tool command rather than a committed frame:

          q2psx-inspect viewweapon <disc> 1 out.ppm BASE0 0 0 ref/captures/base0_blaster.ppm

      It reads a P6 capture, strips the letterbox (both captures are 640 x 480 holding a 640 x 432
      picture — measuring against 480 puts every vertical fraction out by a tenth), finds the blaster's
      EMITTER as the largest strongly-saturated warm blob in the lower right, and reports the box in both.
      The emitter is the landmark because segmenting a whole weapon out of a dark canyon is guesswork
      while that block is unambiguous, belongs to the model rather than the HUD, and is small enough that
      its box is a position and a size.

          console  640x480  band y 22..453  167 px   cx 0.6383  w 0.0266  offset/width 5.21
          port     512x248                   44 px   cx 0.5605  w 0.0195  offset/width 3.10

      **The last column is the argument.** The offset from the projection centre goes as `vx * H / vz` and
      the emitter's width as `s * H / vz`, so their ratio is `vx / s` and the projection cancels out of it
      entirely. 5.21 against 3.10 cannot be explained by a field of view, a viewport, or a display aspect,
      and it cannot be explained by the spawn either — the weapon is attached to the VIEW, so where it
      lands does not depend on where the player stands or which way they face.

      Three things the same measurement clears:

      - **the projection centre agrees.** The reticle in the capture sits at x 0.5023 of the width against
        the port's 0.5000 — a pixel and a half on a 640-wide picture.
      - **the vertical agrees** once the letterbox is off: the emitter's centre is 0.6516 of the active
        band against the port's 0.6452, and its height 0.0417 against 0.0403.
      - **the shape agrees.** Resampled into the same geometry the two silhouettes match — same body,
        same emitter block, same barrel, same arm — so the clip rotation and the model are right.

      A fourth control, and the one that makes the horizontal number trustworthy: **the HUD**. It is drawn
      at fixed framebuffer coordinates and goes through none of the 3D chain, so it measures the picture's
      mapping directly. The health cross sits at x 0.2141 of the width in the capture against 0.2129 in
      the port — so there is no overscan crop and no horizontal offset in the capture, and the reticle
      being dead centre could not have told us that on its own (a symmetric crop keeps the centre and
      expands everything around it). The cross's measured WIDTH does differ, 0.0453 against 0.0215, and
      that is the capture's own blur: it is a video frame upscaled 1.25x horizontally, and a small bright
      sprite bleeds. Which means the emitter's measured width is inflated too — so the console's true
      offset-to-width ratio is HIGHER than 5.21, not lower, and the gap is if anything larger than
      measured.

      So what is left is horizontal placement: the port's emitter sits **0.078 of the picture width** to
      the left of the console's, and that is a real displacement rather than a projection artefact.
      `viewweapon` now prints the weapon's view-space geometry so the two frames can be reasoned about in
      the same space:

          blaster, BASE0 idle:  view x 73..207   z 41..526   grip at (140 161 44)
          shotgun, BASE1 idle:  view x  6..287   z 160..564  grip at (216 245 256)

      **What this is NOT.** Not the projection (`H`, `OFX`, `OFY` all swept and shared with the world),
      not the field of view, not the spawn (the weapon is view-attached), not the animation phase (the
      blaster's idle holds `t` constant and moves only the rotation), not the model, not the rotation (the
      silhouettes match when resampled into the same geometry), not the near plane (fixed), and not the
      pose (applied).

      **AND THE THREE LANDMARKS DISAGREE WITH EACH OTHER, WHICH IS THE REAL FINDING.**
      `Q2_VW_PROBE="x0,x1,y0,y1"` makes the vertex walk report which vertices land in a screen box and
      where they sit relative to the grip, so a blob measured in a picture becomes a model coordinate.
      The emitter's box on the port resolves to vertices at **view x 73..146, z 393..526** — from the grip
      x −67..+6, z +349..+482 — which pins the geometry the comparison needs. With that, three
      independent ratios between the two pictures can be formed, and for a RIGID object under ONE
      projection all three have to agree:

          horizontal offset from centre   console / port = 2.29
          emitter width                   console / port = 1.37
          vertical extent above centre    console / port = 1.15

      They do not. No placement of a rigid model, no field of view, and no depth produces those three
      numbers together — a single scale about the projection centre would make all three equal, a single
      depth change would make the first two equal, and a translation cannot make the vertical ratio
      differ from one while the model is unchanged.

      So the failing thing is the COMPARISON, not necessarily the port. Two candidates, and they are
      distinguishable:

      - **the landmarks are not the same faces.** The console's emitter blob is 167 px against the port's
        44 — nearly four times the area where a 1.37 linear scale predicts 1.9. Blur adds a halo, but not
        that much.
      - **the capture's projection is not the console's.** Both captures are frames of an `.mp4`. If they
        were recorded through an emulator with a widescreen or internal-resolution enhancement, the
        horizontal field of view is altered while 2D HUD elements are left alone — which is exactly the
        signature seen here: HUD positions agree to a pixel and a half, the vertical agrees, and only the
        horizontal does not.

      **A third capture, at native resolution, settles which of the three numbers to believe.** Its picture
      is 583 x 252 inset in a 640 x 480 frame, so both axes need the active band; measured that way:

          emitter   cx 0.6372   cy 0.6508      (the first capture gave cx 0.6383)
          HUD cross cx 0.2127                  (the port's is 0.2129)

      So **positions repeat across two independent captures and the HUD lands on the port's to two parts
      in ten thousand**, while the widths do not repeat and do not agree between axes: the emitter measures
      13.2 framebuffer pixels wide against 9.5 tall where the port's is 10.0 by 10.0, and the HUD cross
      measures twice the port's width. That is video chroma subsampling smearing colour horizontally, and
      it is why the width ratios were nonsense. **Centroids are the measurement; widths are not.**

      Taking only centroids, the picture is clean and single-parameter:

          horizontal   console 0.6372   port 0.5605    apart by 0.077 of the width
          vertical     console 0.6508   port 0.6452    apart by 0.006
          HUD          console 0.2127   port 0.2129    apart by 0.0002

      Vertical agrees, so the DEPTH agrees — a weapon at a different distance would move vertically too.
      The HUD agrees, so the mapping is 1:1 and the viewport really is 512 wide (a narrower buffer would
      put the HUD's own fixed coordinates at a different fraction). What is left is a pure horizontal
      displacement of `vx`, and with the emitter's depth known from the probe (vz ≈ 494) it is
      **about 120 view-space units**: the console places the grip near x 260 where the port places it at
      the 140 the disc's key holds.

      **AND THE 120 UNITS WERE THE ROTATION ORDER.** `RotMatrix` at `0x80089E38`, transcribed element by
      element rather than inferred from two of its nine, composes **Ry(y) · Rx(x) · Rz(z)**:

          m00 =  cy*cz + sy*sx*sz   0x8008A014      m10 =  sz*cx   0x80089FAC
          m01 = -cy*sz + sy*sx*cz   0x8008A044      m11 =  cz*cx   0x80089FCC
          m02 =  sy*cx              0x80089F20      m12 = -sx      0x80089F0C
          m20 = -sy*cz + cy*sx*sz   0x8008A0B8      m21 =  sy*sz + cy*sx*cz   0x8008A08C
          m22 =  cy*cx              0x80089F34

      `q2_rotation_euler` built **Rz · Ry · Rx**, and its own header said not to worry: "the composition
      order is unobservable on this disc — the rotation integrator at `0x8002F1A8` stores to
      `obj[0x0C + 2*axis]` and nothing ever clears the other two slots, so exactly one angle is non-zero
      and the three orders agree." **That was true for a ROTATING BRUSH and stopped being true the moment
      the view weapon became a caller.** A clip's rotation carries all three angles at once — the
      blaster's idle is (2078, 2110, 1985), a half circle either side — and the two orders agree only when
      the roll is zero. An argument that a difference cannot be seen is only as good as the list of things
      looking, and that list grew without the argument being revisited.

      Measured against the capture, before and after:

          port before   cx 0.5605   offset/width 3.10
          port after    cx 0.5869   offset/width 4.94
          console       cx 0.6383   offset/width 5.21

      `tests/test_viewweapon.c::test_rotmatrix_order` pins the five single-product entries by their store
      addresses, so a revert fails.

      **What is still open is the remaining 0.051 of the picture width.** It is a quarter of what it was,
      it survives the idle sweep (the port's emitter ranges 0.5869..0.5996 against the console's 0.6383),
      and it is no longer attributable to anything read so far.

      **THE CAMERA MATRIX IS NOT A PURE ROTATION, AND THAT IS WHERE THE REST OF IT IS.**
      Following the GTE's rotation registers back: `0x800313DC` loads the world draw's matrix from
      **view+160**, and `0x80037F44` builds it —

          0x80055DE4(dst = view+160, src = view+192, vw = view+278, vh = view+280)

      — scaling the source basis by **vw/320 on row 0** (`0x80055DFC`'s `0x66666667` with `sra 7`) and
      **vh/240 on row 1** (`0x80055E8C`'s `0x88888889` with the add-then-`sra 7` form). The port's camera
      is `q2_rotation_view`, a pure rotation, for both the world (`world.c:562`) and every model
      (`modeldraw.c:120`). Whatever that scale is, the port does not have it.

      **The scale is VERTICAL ONLY, which rules it out as the horizontal residual.** `0x800779BC` loads
      the immediate **320** and `0x800779C0` stores it to view+278, so `vw` is a constant and row 0's
      scale is 320/320 = **1**. view+278 and +280 are written exactly once each — no layout overwrites
      them — so that holds for every layout. Row 1 takes `vh` from the caller's `a2` at `0x800779C4`,
      which screen.c reads as 160, giving **2/3**.

      So the console's camera squashes Y by two thirds and leaves X alone. **It was applied, and it is
      DISPROVED as a port change.** `q2_rotation_view_scaled` was added, wired into both camera sites
      (`world.c` and `modeldraw.c`), built and measured on BASE0:

          sky wedge width   port before 0.2988   with the scale 0.6953   console 0.2556
          weapon emitter y  port before 0.6532   with the scale 0.6008   console 0.6508

      The world's vertical field of view blows out — the wedge nearly triples where the console's is
      NARROWER than the port's already — and the weapon's vertical goes from agreeing within 0.0024 to
      missing by 0.05. Both worse, both measured, reverted.

      So the reading of `0x80055DE4`'s effect is incomplete rather than the port being wrong: either the
      basis at view+192 is already scaled to compensate, or the `vh` that reaches the world path is not
      the 160 at `0x80077948`. **The term is real in the instructions and wrong as a naive camera scale**,
      and it cannot be the horizontal residual either, because it does not touch X.

      Which also disposes of a tempting coincidence, recorded so it is not re-derived: the port's emitter
      offset times **1.6** lands within **0.4 of a pixel in 512** of the console's, and 1.6 is 512/320 —
      the row-0 scale one would get by reading `vw` as the viewport width. It is not the viewport width.
      A number that good is worth writing down and worth not believing.

      **Both scales are now settled from immediates.** `vw` is the 320 at `0x800779BC`, and `vh` is the
      **160** at `0x80077948` — the same store the screen check already validates as "projection distance,
      and the 2D height". So the console's camera is

          diag(320/320, 160/240, 1) * basis  =  diag(1, 2/3, 1) * basis

      — **no horizontal scale, and a two-thirds vertical squash the port does not have.** That is a proven
      divergence in its own right and it is bigger than the weapon: it is the whole game's vertical field
      of view. At `H` 160 over a 248-line viewport the port's vertical half-angle is `atan(124/160)`,
      giving the 75.6° FIDERITY §11 quotes; with the squash it is `atan(124/160 / (2/3))`, giving 98.6°.
      **It is recorded and NOT applied**, because the difference between those two is every frame the
      renderer draws, and the case for it rests on two immediates and no measurement. Measuring it wants a
      capture with a known camera — the same thing #46 wants — and it should be settled before, not after.

      And the basis those scales multiply, `0x80055934`, is now transcribed in full. It reads the view's
      own angle triple at view+12 — call them 0, 1, 2 in store order — and builds

          m00 = c2*c1 - s2*s0*s1     m01 = -s2*c0      m02 = c2*s1 + s2*s0*c1
          m10 = s2*c1 + c2*s0*s1     m11 =  c2*c0      m12 = s2*s1 - c2*s0*c1
          m20 = -c0*s1               m21 =  s0         m22 = c0*c1

      with its own ROUNDING — `bgez x; addiu x, x, 4095` before `sra 12`, truncating toward zero where
      `RotMatrix` and the port both floor. **It is orthonormal**, so there is no hidden horizontal term in
      it, which closes the last place a scale could have been hiding.

      **But it is a DIFFERENT COMPOSITION FROM `RotMatrix`, and that is the thing the port has assumed
      away.** The console's weapon reaches the screen as

          camera * entity  =  [diag(1, 2/3, 1) * basis(view angles)] * [RotMatrix(aim+kick) * clip]

      The port instead makes `camera * R_place` the identity by construction and hands the model `clip`
      alone — `test_camera_undoes_the_placement` pins exactly that. **The identity is the port's
      invention.** Two different compositions of the same angles are not inverses of each other, so on the
      console the product is a residual rotation, not `I`, and the weapon does not sit at view-space `t`.

      At zero pitch and zero roll both collapse to a yaw about Y and the residual vanishes — which is why
      `q2psx-inspect viewweapon` renders (pitch 0) show the identity working, and why nothing in the port
      has ever contradicted it. **The captures are not at zero pitch.** So the residual is a candidate for
      the 0.051 that the port's own harness structurally cannot see.

      **The camera is not a third composition — it is the port's own.** Searching every assignment of
      (±yaw, ±pitch, ±roll) to the basis's three angles, `basis(-pitch, -yaw, +roll)` matches
      `q2_rotation_view(yaw, pitch, roll)` with a total absolute error of **18 over 36 elements** on
      values reaching 4096 — half a unit an element, which is the difference between truncating toward
      zero and flooring and nothing else. `q2_rotation_view` is right, and the camera needed no change.

      **THE IDENTITY IS THE CONSOLE'S AFTER ALL, and the way that was settled is worth more than the
      answer.** Evaluating `camera * RotMatrix(aim+kick)` on the assumption that the camera's angle triple
      is the weapon's produces a residual: the identity at pitch 0, and off it a grip whose DEPTH collapses
      from 44 to 13 at pitch 60 and grows to 98 at pitch −120. That looked like a real divergence, so it
      was IMPLEMENTED — `q2_vw_place` rewritten as `RotMatrix(ang)` applied forward, `q2_vw_build_ot` as
      `MulMatrix(view, clip)` with no cancellation, both tests rewritten to match.

      **The renderer falsified it in one frame.** Looking up, the arm stretches across the screen; looking
      down, the weapon leaves the frame entirely. No version of this game does that. So
      `camera * RotMatrix(view angles) == I` on the console, and the port's construction is right.

      The premise was the error. The mapping search found `basis(-pitch, -yaw, +roll)` matching
      `q2_rotation_view(yaw, pitch, roll)` — **note the negated yaw and pitch**. The camera's triple at
      view+12 is the INVERSE convention of the weapon's `aim + kick`, which is precisely what makes the
      cancellation exact. Evaluating the product with the same signs on both sides was comparing a matrix
      with itself rather than with its inverse.

      So: the identity is confirmed rather than invented, it is not a divergence, and it is not the 0.051.
      Reverted, with the disproof recorded here so the next reader does not re-derive the residual and
      believe it.

      **Re-measured through the CLIENT, which is the renderer that matters, and the number is smaller and
      the asymmetry sharper.** The tool renders at pitch 0 with a synthetic aim; the client runs the real
      thing. Over 240 frames of BASE0 the client's emitter sits at

          client   cx 0.5947   cy 0.6532      (stable across the whole idle)
          tool     cx 0.5869..0.5996          (the client is inside the tool's range — they agree)
          console  cx 0.6383   cy 0.6508

      **Vertical agrees to 0.0024 — six tenths of a pixel in 248 — and horizontal is out by 0.0436**, or
      22 pixels in 512. That asymmetry is the whole remaining problem, and it is a strong constraint: with
      `H`, `OFX` and the depth all shared between the two axes, a term that moves x while leaving y alone
      has to be `t.x` itself or the model's own x. `t` is `(140, 157, 44)` on every key of the blaster's
      raise and idle, at key offset +6/+8/+10, which `0x8004F494`, `0x8004F508` and `0x8004F57C` confirm
      and the tool's constant check pins.

      **And the renderer agrees with the model of it.** `viewweapon` now projects the weapon's vertices by
      hand — clip rotation, clip translation, `H` about the viewport centre — and prints the box beside
      the one taken from the primitives `q2_vw_build_ot` actually emitted. Two independent routes:

          predicted from the vertices   292..1084
          emitted by the renderer       292..665

      The left edges are identical. The right differs only because the near vertices overflow the GTE's
      divide and clamp, which is the hardware's own behaviour and not a disagreement. So the renderer does
      what every argument here assumes it does, and the transform is not being mis-applied somewhere
      between the model and the primitive.

      So the horizontal remains unattributed, with the projection centre, the field of view, the spawn,
      the animation phase, the model, the near plane, the pose, the rotation order (fixed), the camera's
      row-0 scale, the camera composition, the identity residual, the vertical camera scale (applied and
      disproved) and the renderer's own fidelity to the transform all eliminated. **Thirteen candidates,
      five of them applied and measured worse.** What is left has to be `t.x` or the model's own x, and
      both are read straight off the disc. `q2_rotation_view`'s own doc is known-stale as well: it
      justifies applying the roll outermost from `RotMatrix` composing `Rz * Ry * Rx`, which it does not.

      That is as far as measurement can take it. `t.x` is 140 on every key of the blaster's raise and idle,
      `ApplyMatrix` is `(M·v) >> 12` with the shift at `0x8006FE08`, `view * R_place == I` is pinned by a
      test, and the block at `0x8004F644` that could have carried a lateral offset is a zero vector. The
      120 units are not in any operand that has been read, so either a term reaches the transform from
      somewhere not yet found, or the remaining captures still carry an enhancement. **The next capture
      should be a DuckStation screenshot rather than a video frame** — PGXP off in all its forms, widescreen
      hack off, resolution scale 1x, crop None, aspect 1:1 — which removes the chroma smear and lets the
      silhouette, not one blob, carry the measurement.

---

- [x] 50c. **The view weapon takes the WORLD lighting branch; `+0xF4 = 1` was
      misread as a boolean instead of a signed selector.** The decisive instruction is the one immediately
      after the load: `lh v0,244(s2); bgez v0,0x8006B08C` at `0x8006B038`. Non-negative values take the
      world dynamic list and the node's static lamps; only a negative halfword takes the alternate list.
      The constructor's literal **+1** at `0x8004F750` therefore proves that the client's world gather was
      the right mechanism. The dark-grey experimental render was not evidence for a missing view-list
      producer; it was the exact visible consequence of inverting `bgez`.

      **The actual client error was the ambient floor.** The entity allocator writes
      `0x40/0x40/0x40` to both ambient colours at `0x8006C1D8..0x8006C1FC`; nothing in the viewmodel
      constructor overwrites them. The client guessed the item spawner's separate `0x30` constant. The
      viewweapon now retains its real `+0xFC`, `+0xFE`, `+0xF4` and `+0x2AC` fields, folds the two intensity
      factors into lighting, passes literal +1 through a signed gather API, and uses the allocator's
      0x40 back colour. `tests/test_light.c` pins all three arms of the signed test (+1, zero, -1), and
      `tests/test_viewweapon.c` pins the constructor state.

      Deterministic BASE0 frame 119 changes only the weapon coverage: the violet body and arm gain the
      allocator's missing ambient while the world and HUD remain identical. This closes the lighting
      mechanism; the later arm-material discrepancy is the separate VRAM/CLUT question recorded near
      #131, not a reason to invert a branch whose instruction is unambiguous.

- [x] 50a. **The view weapon queues something white and 128 units wide at its own position — and the
      gate it is behind is NEVER WRITTEN, so it never happens.** `0x8004F6CC` calls `0x8007012C` with the
      weapon's finished world position, `128`, and the four bytes at `0x800AEA20`, which are `ff ff ff 00`.
      `0x8007012C` appends a twenty-byte record to a bump list between `gp+18496` and `gp+18500` — three
      position words, the `128`, the packed colour. It is NOT `q2_light_add_dynamic` (`0x80075C34`,
      28-byte records at `0x800E3D18`): the consumer at `0x8006EBB8` walks the list from `0x800DEEA0`,
      subtracts the camera at `0x800B28B4` and builds 48-byte primitives, so these are **billboards** and
      the `128` is a size. `0x80050B84` queues one from a muzzle position with the colour at `0x800AEA28`.

      **The whole thing is behind `0x800B2A28`, and nothing in the executable writes it.** Three sweeps
      agree: no `gp`-relative store at its offset, no `lui`+`addiu`/`ori` materialising its address, and
      no load/store through a register holding a base that lands on it — the last of these tracks
      materialised pointers through `addiu`/`ori` and clobbers them at calls, so it catches the
      `p = &block; sh x, k(p)` form the first two miss. Five sites read it as a halfword
      (`0x8002F48C`, `0x8002F920`, `0x8004F688`, `0x80050B84`, `0x80054AE4`) and none writes it.
      It sits at `0x800B2A28`, past the text segment's end at `0x800B2800`, so it is BSS: zero at boot and
      zero for ever.
      **So the port is already at parity by drawing neither**, which is a stronger answer than "not
      implemented" and the same shape as the glint: a finished effect the shipped build never turns on.
      What could still change it is a relocated level module writing through its own base; nothing in the
      boot image can.

- [x] 50b. **The quad's firing sound — WIRED, and the sound is `itm_damage3`.** Four sites do the same
      thing after a shot that succeeded: `0x8004FB9C` in the state machine's own fire arm, and
      `0x80050004`, `0x80050234` and `0x80050300` in the machinegun's, chaingun's and hyperblaster's.
      Each tests the level clock at `0x800AEBAC` against the deadline at `combat+172`, asks `0x800739B8`
      whether the handle at `0x800B2B80` is already sounding, and if not plays `[0x800B28B0]` at the
      player's position through `0x80073734`.
      `0x800B28B0` is filled at `0x80037AA0`, and the name loaded for that call is at `0x800AC2AC`:
      **`itm_damage3`**. It is not one of the twenty-two `wep_*` sounds the weapon table holds, which is
      why it needed its own signal rather than riding the existing one.
      `combat+172` is `q2_inventory.quad_until` and the clock is `q2_sim.level_time`, both of which the
      port already kept — so the comparison is the console's and only the plumbing was missing.
      `q2_vw_take_quad_sound` raises it at all four sites and the client plays it by name.

- [x] 49. **"The player cannot damage a creature" — RETRACTED the same day it was
      written. They can; the test could not see one.**
      The measurements in the original entry were all correct and the conclusion drawn
      from them was not: 195 attack ticks, bolts in flight, 22 targets registered,
      matching hit radii, actors synced both ways, and creature health unchanged. What
      none of them measured was whether a bolt had anywhere to go.
      Two faults in the test, and the second only showed up once the first was fixed.
      The aim was being written AFTER `q2_sim_advance`, and the shot is taken inside
      it, so an aim applied at the end of a frame governed the frame after the one
      that fired — the run was firing wherever the demo happened to face. Correcting
      that turned every bolt into a floor impact instead, which is the second fault:
      on BASE1 the nearest creature is a storey below, so a correctly aimed shot goes
      into the floor between them. **That is geometry, not combat.**
      `--watch` now stands the PLAYER in front of the creature as well as the camera —
      700 units along its own facing, at head height — and with a clear line the
      player kills: eight creatures become seven, 200 total health becomes 180, in
      under 250 frames.
      The lesson is worth more than the item: every step of the chain was instrumented
      and every number was right, and the conclusion was still wrong, because the one
      thing not instrumented was whether the experiment was capable of a positive
      result. A test that cannot succeed reports the same numbers as a broken feature.

## The port's real gap is missing callers, and here is the list

Four separate finds this session were the same shape — a finished piece with no
caller: the overlay initialised from a flag that had not been set, the creature
action hooks nothing ever assigned, the free-fly camera a session booted into, and
the death screen. That is a pattern rather than four coincidences, so it was worth
measuring instead of noticing.

Sweeping every `q2_*` and `psx_*` function declared in a header and counting calls
across all of `src/` and `tools/` gives **89 that are never mentioned anywhere but
their own definition**. Most are honest accessors. Some are whole subsystems.

**The first version of this sweep said 100, and it was wrong in a way worth
recording**, because it is the same mistake as trusting a measurement without
asking what it can and cannot see. It counted `name(` — call syntax — so every
function installed as a FUNCTION POINTER looked dead. Eight are: the item thinks
`q2_item_think` and `q2_item_shrink_think`, and the AI verbs `q2_ai_stand`,
`q2_ai_walk`, `q2_ai_run`, `q2_ai_move`, `q2_ai_charge`. Those are wired and always
were, and the table below no longer claims otherwise. Counting bare identifiers as
well separates the two.

| what | evidence |
| --- | --- |
| the **multiplayer runtime** | eleven functions — `q2_mp_session_init`, `q2_mp_frame`, `q2_mp_player_killed`, `q2_mp_find_winner`, `q2_mp_banner`, `q2_mp_score_title`, `q2_mp_team_name`, `q2_mp_may_respawn`, `q2_mp_attribute_kill`, `q2_mp_hud_image`, `q2_mp_take_request` — the whole `QMULTI.C` reconstruction, with nothing to drive it |
| the **rotating brushes** | `q2_rotators_build` had one caller and it was an inspector command, so ROTHATCH, SIMROT, SIMROT2 and ROTBUTTON were never even constructed in the game |
| the **AI breadcrumb trail** | `q2_trail_init` and `q2_trail_add`, so the ring at `gp+17892` was always empty and a creature that lost you had nowhere to follow you to |
| **`q2_monster_damage`** | the module-owned health path, which is how a creature with an AI brain is meant to take damage |
| the **per-frame lighting** | `q2_light_world_begin_frame`, `q2_light_glow_fade`, `q2_light_env_apply` — and the client passed a NULL light world with `coll_node = -1`, so nothing that is not the world was lit at all |
| the **view weapon's own outputs** | `q2_vw_take_refire`, `q2_vw_take_event`, `q2_vw_wants_fire` — so running a gun dry never switched off it, and an animation event never reached the frame it belongs to |
| **`q2_weapon_autoselect`** | what a pickup is meant to consult |

**The lights are wired now**, and they were two separate omissions rather than one.
The client passed `ectx.lights = NULL`, and it also passed `ectx.coll_node = -1` —
"no node" — so even a light world would have handed every entity the fallback. Both
come from what the sim already tracks: `Lights` out of COMMON.DAT, `SpaceLights`
opened against the same `SecondaryCol` the sim uses (that being what partitions it,
FORMATS §17), and the player's own cell.
Items pick it up through the entity draw; creatures did not, because the creature
loop calls `q2_model_build_ot` directly, so it gathers its own three — three being
all the GTE's light matrix has rows for.
The result is strongly coloured and that is the map, not a fault: `q2psx-inspect
lit BASE1` reports the accepted lights in zone 0 as `rgb 2988 1992 937`,
`1447 964 457` and `912 415 0`, so a Soldier standing in that room comes out
orange. Checked rather than assumed, because the change is large enough to look
like a bug.

Three more are now wired. The view weapon's refire signal drives the auto-switch off an
empty gun, and its event is drained on the frame the clip raises it rather than the
frame the trigger was pressed — and the verdict handed back to the machine is now
`last_shot.dry` rather than an unconditional "it fired", since `fired` is also
false during an ordinary refire wait and would have reported every shot as denied.
The client builds and ticks the rotator set — BASE0 has two
rotators, BASE1, COMMAND and POWER1 one each, JAIL2 none — and drops a breadcrumb
every ten frames. *Still open for the rotators:* nothing triggers a step. A
rotator moves when SIMROT's exec calls `q2_rotator_trigger`, and the event runtime
has no primitive-dispatch hook to reach it from, so `q2_rotators_tick` reports zero
moved. Built and ticking is not turning, and the log says which.

— **the hook now exists** (`q2_event_rt.on_call`) and `q2_rotator_trigger` is called through
`q2_rotators_call`; disc-wide, 46 steps turn 47 rotators. The client still reports zero, for a
different reason: the demo pad had not walked into a volume that fires one. It does on LAB —
`rot 1 steps 0 moved 1 turned` — and `rot moved` was the wrong statistic anyway, since a SNAP
takes its whole rotation at trigger time and never tick-moves. See question 50 below.

## Nothing had ever opened the death screen

Page 41 has been transcribed since the menu was reconstructed — `RESTART LEVEL`,
the resupply line with its own greying rule at `0x8001D774`, `QUIT GAME` — and no
caller ever opened it. So the player's health simply ran negative and the game
carried on: measured before the fix, a Soldier took the player to **-353** and the
run continued as if nothing had happened. It is the same shape as the overlay that
was never initialised, the creature hooks nothing ever set, and the free-fly camera
a session booted into — a finished piece with no caller.

It is raised from the client rather than the sim, because the sim has no menu and
the page IS the death sequence here: the world freezes behind it, which is what
every other page already does.

That immediately exposed a second thing. A scripted run could not answer the page —
`client_menu_pad` reads the keyboard — so the death screen ended every headless
run: the world frozen, the demo's pad going to a simulation that was no longer
ticking, every later frame identical. The demo now answers a page with CROSS on a
slow cycle, which takes the row a page opens on, and a run dies, restarts and
carries on.

## The creature modules name their own animations

Chasing the sound table turned up a third table beside it, and it is the more
useful of the two. Every creature module carries **three** blocks of text: its own
name at about `+0x168` (`Soldier`, `Tank`, `Arachner`, `Gunner`, `Insane` — note
that the module calls it `Tank` where the class table says `Tankcomm`), the
12-byte sound-name table at about `+0x1C4`, and a **move-name table** further in.

A move-name record is twenty bytes:

        +0   char name[16]     NUL-padded, and NOT terminated when it fills the
                               field — `Attak 1 Loop` is exactly twelve
                               characters, the same rule the level table and the
                               `Strings` dictionary use
        +16  u16 first_frame
        +18  u16 last_frame

The two frames are the move's own range, which is what ties a name to a move
**without depending on the table's order** — and that matters, because the order
is not the decoder's: the decoder finds moves through whichever callback reached
them first. Matching by range instead, `q2psx-inspect creatures` names **83 of the
disc's 97 moves**, including the Soldier's, which a first look at the string pool
had wrongly suggested carried none.

So a creature's animation set is self-describing: `Start Walk` 34-49, `Attak 1
Pre` 65-70, `Attak 1 Loop` 71-76, `Attak 1 End` 115-135, `Stop Walk` 222-253. That
is what a port needs to say which move is which without inferring it from a
callback slot, and it is on the disc.

## Two things the creature chain was still missing

**A correction to this section's own first version.** The sound hook was wired with
`cre_pain1` and `cre_die1` for indices 0 and 1, and that was an invention twice over:
the bank has no such names, so it silently played nothing, and the index-to-name
mapping had never been read. It is gone.
What replaces it is the module's own table, which turned out to be on the disc all
along. A creature module carries **two parallel tables**: eleven resolved handles at
`module+0x32A0` on a 4-byte stride, zero on disc because the engine fills them at
load, and eleven 12-byte NAME fields at `module+0x1D0`, which are not:

        0  sol_idle1     4  sol_pain3     8  wep_machgf1b
        1  sol_sght1     5  sol_deth1     9  wep_shotgf1b
        2  sol_pain1     6  sol_deth2    10  msc_udeath
        3  sol_pain2     7  sol_deth3

They share one index, and `cre_soldier.c` already recorded three handle addresses
against its enum — which is what makes this checkable rather than merely plausible.
All three agree: `SOL_SND_COCK` at `+0x32C4` is index 9, `wep_shotgf1b`, a shotgun
guard's own fire sound. **And the two the enum had guessed were wrong**: it numbered
pain and death 3 and 4, following on from the three it knew, where the table says 2
and 5 — 3 and 4 are `sol_pain2` and `sol_pain3`.
Every one of the eleven is in the map's bank, so the port now plays them. Another
creature's table has not been read, so those stay silent rather than borrowing the
Soldier's.

**Nothing had ever set the action hooks.** `crebind.h` defines a sound hook, a
fire hook and a melee hook, and the only definitions of the setters were in
`cre_soldier.c` — no caller anywhere in the tree. So every claw, every shot and
every sound a creature made went to a null pointer: they chased the player and
could not touch them. The melee and sound hooks are now wired in the client. The
FIRE hook deliberately is not: a module's melee carries real decoded figures (the
Arachner's is `aim 1020,-48,0 dmg 20+r%5 kick 100`) but its shot reaches an
indirect call the action decoder reports as `call(+D8)?`, so what damage a
creature's gun does is not read yet (#6), and an invented number would make every
creature in the game lethal on a guess.

**Where a creature's shot damage actually lives — a redirect for #6.** It is not in
the module. `soldier_fire` at `module+0x1120` picks one of three 8-entry tables by
skin (`module+0x3240`, `+0x3260`, `+0x3280`) and the entries are **small integers**
— table 0 reads 39, 40, 83, 86, 89, 92, 95, 98 — which are muzzle-flash indices,
not damage. It then calls engine slots `+0xC8` and `+0xD8` with vectors.

      **A retraction, one step old.** This entry briefly said the figures were
      "reachable from `0x80078288`", the callee of `+0xC8`. They are not:
      `0x80078288` indexes the CAMERA array at `0x800D5C30` with a 784-byte
      stride, reads its `+262`/`+266`/`+268`, and ends by loading the GTE's
      `TRX`/`TRY`/`TRZ`. It is a view setup. The call being traced was placing
      the muzzle in view space, not firing, and the inference from "the fire
      function calls this" to "the damage is in this" skipped the step of
      establishing WHICH of the calls is the shot. `+0xD8` resolving to
      `0x800B2C2C` — a data address, not a function — is the other half of the
      same warning: the base register in that fragment was never shown to be
      the engine block.
      So the shot figures remain unlocated, and the honest statement is narrower
      than the one it replaces: they are not in the flash tables, which hold
      muzzle-flash indices, and the module's own image does not carry them as
      immediates. Where they are is open.

      **What the second attempt did establish is general, and worth more than the
      one slot it was after.** The right indirection was never the LevelBin engine
      vtable — a CreAI module reaches the engine through its OWN 71-pointer import
      table at `module+0x14`, and the loader that fills it is at **`0x8007DA00`**,
      writing every slot individually as `sw v0, N(s0)` with the address
      materialised two instructions above. So any slot can be named by grepping
      that one function for its offset, and the census's `call(+XX)?` reports stop
      being opaque numbers. The method checks out on the slot already known:
      `+0x28` resolves to `0x8006FC1C`, the SVECTOR rotate, which is
      `Q2_IMP_LOCAL2WLD`.
      Applied to the Soldier's fire think, whose actions decode as
      `call(+D8)? call(+2C)? call(+28)? call(+C8)? call(+D8)? call(+C0)x3`:
      `+0xC0` is `0x8005C460`, a per-component scaled vector add
      (`out = a + (s*d) >> 12`), so **the three trailing calls are muzzle
      arithmetic, not three shots** — which is the specific wrong reading the
      retraction above was heading towards. `+0xC4` is `0x8005C634`, `+0xEC` is
      `0x80061118` — the `fire_hit` the melee already uses — and the two that
      were unread are now read: `+0xC8` (`0x8005F934`) is **`vectoangles`**,
      ratan2 through `0x8008A358` with the horizontal length through
      `0x8008A7E8`, returning 3072 and 1024 for straight down and up; and
      `+0xD8` (`0x8005BB58`) is **angles to vectors**, three angles masked to
      `0xFFF` indexing the packed `{sin, cos}` table at `0x800A5430`.

      **The shot was found, and it was one indirection further out.** `soldier_fire`
      does not reach it through `s3`, the register the decoder follows, but through a
      fresh `lui` — `lw v1, 128(v0)` with `v0 = module base` — which is import
      `+0x80`. The loader at `0x8007DA00` names the whole family, and it is
      contiguous, exactly as id's is:

          +0x80  0x80062000  monster_fire_blaster
          +0x84  0x80061DFC  monster_fire_bullet
          +0x88  0x80061ED0  monster_fire_shotgun

      And the Soldier's three arms carry their figures as immediates:

          skin < 2   blaster    damage 5, speed 600
          skin < 4   shotgun    damage 2, kick 1, spread 1000/500, 12 pellets
          otherwise  bullet     damage 2, kick 4, spread 300/500

      **Every one of those is id's own number** — `monster_fire_blaster(…, 5, 600, …)`,
      `DEFAULT_SHOTGUN_HSPREAD` 1000 / `VSPREAD` 500 / `COUNT` 12,
      `DEFAULT_BULLET_HSPREAD` 300 / `VSPREAD` 500 — which is the check that says the
      read is right rather than merely self-consistent. The port's fire hook now
      carries them, and a creature whose table it does not know is dropped rather
      than handed a Soldier's gun.
      *Measured, not assumed:* on BASE1 over 1800 frames with four creatures hunting,
      the hook is invoked **zero** times. **The root cause is found and it is a
      one-line guard in the port.**
      The frame driver dispatches a frame's think through the class method table, and
      the Soldier's methods are installed, so that path was never the problem. What
      stops it is one step earlier: `q2_ai_checkattack` ends with
      `if (!m->checkattack) return false;`, and **the original does not test it** —
      `0x8005E320` is `lw v0, 260(s1)` and `0x8005E328` is `jalr v0` with nothing
      between them. `entity+0x104` is never NULL on the console, so the engine
      installs a default at spawn that a module may override.
      **No creature module on the disc installs one.** The Soldier's callbacks are
      stand, walk, run, dodge, attack, sight, pain and die, and the other six are the
      same. So a guard that reads as ordinary defensive coding disables every attack
      in the game — which is why creatures chase and never fire, and why the melee
      hook never fired either.
      **The default is `0x8005D8C8`, and it is installed at `0x80061B18`.** Found by
      scanning the whole text segment for the instruction rather than guessing at
      neighbourhoods: `sw rt, 0x104(rs)` appears **ten** times in the image, six of
      them with `rs = sp` (stack frames), one in the module import loader where
      `+0x104` is an import slot rather than an entity field, and exactly one with an
      entity base in the monster spawn — `0x80061B18`, guarded by
      `bne a0, zero` two instructions above, so a caller can suppress it.
      `0x8005D8C8` is `M_CheckAttack`: it reads the enemy through `entity+0xBC`,
      tests its health at `+0x108`, builds the two eye points from `+0x00..0x08` and
      `+0x4C`, and traces between them with contents mask `0x0200001B` before the
      range and random decision.
      **Transcribed, and the creatures now attack.** On BASE1 the same run that
      measured zero hook calls now reports **135 shots and the player at -353 hp**;
      health falls from 100 as soon as a Soldier has line of sight. Every constant in
      it is the original's and every one is also id's, which is the check that the
      read is right rather than merely self-consistent: the four chances are 1638,
      819, 410 and 82 out of 4096 — 0.4, 0.2, 0.1 and 0.02 — skill 0 halves them and
      skill 2 or more doubles them, and the flyer's sliding roll is 9830 of 32768,
      which is 0.3.
      **The blind-fire branch is dead code on this build, and the port returning false
      there is what the console does rather than a narrowing** — which corrects the
      caveat first written here. Its first gate is bit 17 of `entity+0x138`
      (`0x8005D9EC`: `srl 17; andi 1; beq`), and scanning the whole text segment for
      writes to that word finds eight outside stack frames: four are `M_CheckAttack`'s
      own attack-state stores, which mask with `0xFFE3FFFF` and so PRESERVE bit 17;
      one is `ai_checkattack`'s; one writes bits 21 and up; and the two `sh` sites
      reach only the low halfword. **No instruction in the image ever sets bit 17.**
      The blindfire flag is always clear, the branch cannot be entered, and
      `blind_target` — which the port does write, in three places — is read by
      nothing that can run.
      Its shape, for the branch that remains:

        - the first gate is the ENEMY's health, reached through `entity+0xBC` then
          that object's `+0x24` then `+0x108`, and `blez` leaves immediately;
        - it builds two eye points, self and enemy, each as the position triple at
          `+0x00..0x08` with the view height at `+0x4C` added to the middle one;
        - it calls `0x8005BD3C` — a trace — **twice, with different masks**:
          `0x0200001B` for the first and a bare `0x02000000` for the second, the
          second starting from `self+0x5C` rather than the eye, which is the
          blind-fire target `blind_target` at `+0x5C` (§9.12);
        - between them are five or six further gates on the enemy's flags, on
          `self+0x138` bit 17, on `self+0x1C` against 201, and on `self+0x110` and
          `self+0x124` against the global at `0x800E46DC`.

      The guard in `q2_ai_checkattack` stays until all of that is transcribed rather
      than some of it. A checkattack that returns true too readily is worse than one
      that never does: it would put invented aggression on every creature in the game
      and look like a working feature while doing it.

      The earlier statement that follows was written before this and is kept because
      the reasoning it records is still what eliminated the call route:
      **there is no fire call in the Soldier's fire think's IMPORT LIST.** Every one
      of its six distinct imports is aiming arithmetic. That eliminates the whole
      call route rather than narrowing it, and leaves one candidate standing: the
      think loads `s7` from the flash table at `0x80101194` and the tables hold
      INDICES, so the shot is most likely POSTED — a field written for an engine
      pass to act on — which would put the damage in an engine table indexed by
      those flash numbers. That is a hypothesis and is labelled one: neither the
      store nor the table has been found. What is established is that looking for
      a call was the wrong search, and the decoder's own step kinds say what to
      look for instead, since it already classifies stores to `entity+0x138`,
      `+0xD8` and `+0xDC` as their own actions.
      The port leaves the fire hook unset rather than guessing, and a Soldier
      chases and swings but does not shoot.

**Eight of the disc's levels ship an EMPTY `CreAIBin` — four bytes — and place
creatures anyway.** JAIL2, JAIL3 and JAIL4 have Infantry; SECURITY, WASTE2,
BIGGUN, BOSS1 and BOSS2 likewise. Their spawn records name classes the class
table resolves perfectly well, and the port placed nothing at all on them because
the module was missing. The census had the answer and it was read as a
convenience: fifteen module instances across the disc, **seven distinct**,
deduplicated by name. A module of a given name is the same wherever it appears —
the same argument that settled `QMULTI.C`, byte-identical on all thirteen arenas
— so a map with none borrows from one that has them.
JAIL2 goes from 0 creatures to 5, BOSS1 to 2, BIGGUN to 3; BASE1, which ships its
own, is untouched at 22 of 22, which is what says the borrow only fires where it
is needed. The assumption is written into `creworld.c` rather than buried.

## Putting the creatures in the client

Every piece of the creature chain existed and nothing joined them up. The modules relocate, decode and bind;
Population's records spawn; the AI runs — and the only caller was the inspector, which decodes a module to
report on it and draws a creature standing still at its spawn point. A level in the client was its geometry,
its items, and nothing that moves. `src/game/creworld.[ch]` is the join, and making it work turned up three
things worth recording and left two open.

**Three numbers name a creature and they are not the same number.** A Population spawn record carries a
**class id** 0..37; the executable's class table turns that into a **name** and a health, and names are not
unique — ids 18, 19 and 20 are all `Soldier` with 30, 20 and 40 health; and a module serves one or more
**class bytes** 64..94, the Soldier's being 87, 89 and 88 *in that order*. The module is found by matching its
header name against the class table's name, which is the direction the engine's own lookup runs. The variant
is taken from the id's ordinal among the entries sharing the name — three ids, three class bytes, in the order
both tables state them. That last correspondence is inferred from the two tables agreeing in length and order,
not read out of code, and it is the one inference in the module.

**`q2_creature_spawn` overwrites `class_id` with the class BYTE**, because that byte is what the runtime bind
is keyed on. After spawning there is nothing left on the monster that indexes the class table, so anything a
caller wants out of that table — the model name above all — has to be taken while it is still there.

**A creature's line of sight runs through `PrimaryColl`, not `SecondaryCol`.** `SecondaryCol` is `PrimaryColl`
eroded by the *player's* 286-unit half-extent, and a Population spawn point sits in exactly the part the
erosion cuts away: look a creature up in it and it lands outside every cell, so every trace from it fails and
every creature on every map looks straight through the player. In `PrimaryColl` they are inside a cell and
sight works — on BASE1, twenty creatures, sixteen of which acquire the player with no walls in the way and two
across the real geometry.

**Population is per MAP and a session is in one ZONE**, and a spawn record carries no zone field, so the test
has to be geometric: a creature inside no cell of this zone's hull belongs to another one. It is not a small
correction — **twelve of BASE1's twenty**, seventeen of BASE2's twenty-eight and seven of COMMAND's eight
stand in another zone's rooms, and without the test they think, are drawn and are shootable through the void.
Single-zone BASE0 loses none of its ten, which is what says the test is measuring the right thing.

- [x] 47. **Which CastList clip a creature's move plays. — SOLVED: none of them, and none is the point.** A module's moves are numbered in one global frame
      timeline — the Soldier's run 0..474, and `q2psx-inspect creatures` now prints every move's range —
      while its model carries a list of clips, 31 of them for the Soldier. Those clips are **not** that
      timeline laid end to end: they total 434 frames against the module's 474, and there are 31 of them to
      the module's 18 moves.
      What they are is the **moves themselves**. Every one of the Soldier's clip lengths is exactly three
      ticks per frame times some move's length, and clips 1..4 are the four consecutive moves 50-54, 55-61,
      62-79 and 80-96 *in order*. So the tick rate inside a clip is 3 — not `Q2_MODEL_TICKS_PER_FRAME`'s 10,
      which is the view weapon's — and `q2psx-inspect model <map> <name>` now prints every clip's length so
      the arithmetic can be checked on any creature.
      **The index does not exist, and that is the answer.** `0x8006B924` is the selector, and it does not
      index anything: it holds the animation position in a halfword at `entity+0x100` and the current clip
      at `model+0x34`, and *while* the position is past the clip's length it advances the pointer by that
      clip's own `next` byte delta — through `0x80070188`, which is the two-instruction `*p += d` — and
      subtracts that clip's `frames`. A model's clips are therefore **one continuous timeline** and the
      animation position is an offset into it; a clip boundary is wherever the subtractions fall.
      So the question "which clip does a move play" was malformed. A move's frames are positions on the same
      timeline and the walk lands in the right clip on its own, which is also why the measured coincidence
      held: clips 1..4 being the four consecutive moves 50-54, 55-61, 62-79 and 80-96 *in order* is what a
      shared timeline looks like, not a lucky run of matching lengths.
      The three-ticks-per-frame scale survives unchanged, and it is still measured rather than read.
      `q2_model_anim_at` implements the walk and the client's creatures are drawn through it; the
      match-by-length heuristic it replaces is gone.

- [x] 48. **A creature's movement has no hull to run against — SOLVED, and it was never about the hull.**
      The symptom was that a creature acquires the player, animates, and moves zero units for as long as you
      care to watch. The diagnosis blamed the erosion: a box move wants a hull eroded by the MOVER's own
      extent and the disc ships exactly one erosion, the player's.
      That was wrong, and the thing that settled it was counting rather than reasoning. Instrumenting
      `q2_ai_world_bind` says **1052 of 1052 traces** on BASE1 took the "could not place the start" arm, and
      **84 of 84** ground probes reported a drop — under creatures standing on a floor. Numbers that
      absolute are not geometry.
      `q2_coll_move` **returns false when the move was STOPPED**, not when it could not begin
      (collision.h §0x80044C44); `out_pos` is filled in either way and `out_node` is -1 only when the walk
      never found a cell at all. A stopped move is the normal and useful answer for a walker's step trace,
      which exists precisely to be stopped by the floor. The binding read that false as
      `startsolid`/`allsolid`, and `SV_movestep` bails on `allsolid` before it ever looks at the fraction —
      so every creature in the game was told it was buried in the floor it was standing on. `check_bottom`
      had the same inversion: a probe that is stopped has FOUND ground, and it was reporting failure.
      With the contract read correctly the same BASE1 run goes to **1 unplaced trace of 30, no ground-probe
      failures, and 6,522 units of creature movement**; BASE2 and COMMAND move too. And the hull question
      answers itself in passing: `PrimaryColl` is right for all three questions — with `SecondaryCol` it is
      214 of 214 unplaced and 412 of 432 sight lines blocked, because that is the hull a creature genuinely
      is outside.
      `src/game/aiworld.[ch]`; the counters are kept, and the client prints them next to every capture.

## Rotating brushes turn, and the script that turns them was the one we were already running

The rotator set built last round never moved: `rot moved 0` on every map. Not a bug in the rotation — every
kind returns early unless a step is pending (`0x8002F1B8`), and the step is consumed after one step
(`0x8002F204`). One request, one step, and **nothing was requesting**. `q2_rotator_trigger` had no caller.

The request comes from a script `CALL`, and `Q2_EVOP_CALL` fell through the event runtime's `default:` case,
counted with FX and WAIT as "recognised but not implemented". It is now reported to the owner rather than
interpreted: `q2_event_rt.on_call` hands over the item and its UserFuncs index, because *which index is
SIMROT* is a per-map question that `events_rt.[ch]` has no map to answer. What the operands mean lives in
`q2_rotators_call`, beside the builder that reads the same offsets — they differ per primitive (SIMROT names
four objects at +12..+18, ROTHATCH one at +18, ROTBUTTON one at +10) and two copies of that table would rot
apart and turn the wrong geometry.

**In the client, playing:** LAB, demo pad, 900 frames — `rot 1 steps 0 moved 1 turned, 4 calls`. The player
walks into a volume, the volume fires a record, the record calls a rotation primitive, and a rotator that was
standing at zero is standing turned. Across the disc, firing every trigger volume once — a player who has
walked every map — **26 rotators built, 552 CALL items run, 17 rotation steps, 13 rotators turned.**

- [x] 50. **What fires a zone's Events records? — NOTHING, and that is the console's own behaviour.**
      RETRACTED, same day: this was written up as "the single largest piece of level behaviour still missing"
      on the strength of a zone's Events chunk carrying 2959 CALL items, 805 movers and 619 zone gates that
      the port never ran. **The engine never loads that chunk.** The zone loader (0x8007B3F8) looks its
      chunks up by name — PrimaryColl, SecondaryCol, PrimaryRemap, Scene, Points, MapMod, MapNames, SortData,
      SpaceLights, AreaConx, CastList, CreAIBin, CreAIRel — and `Events` is not among them. The image holds
      exactly one copy of the string "Events" (0x800AD480) and exactly two references to it: COMMON's loader
      at 0x8007AC30, whose match stores the chunk pointer into the events global 0x800AE774 (0x8007AD54), and
      the teardown that clears the same global (0x8007C250). Every reader of that global — the load-time
      pre-pass at 0x80026DC0, the execution dispatch at 0x80027950 — therefore reads COMMON's script and only
      COMMON's. A zone's Events chunk is build output the retail engine ignores; 21 of 74 are byte-identical
      to their map's COMMON one and 53 differ, and it makes no difference either way. **The port was already
      running the right script.**

**Two counting tests were run before that and both decided nothing** — recorded so neither is repeated. All
834 trigger offsets start a record in COMMON's script *and* in a zone's (an offset is just a number, and
record starts are dense); and none runs past the end of either chunk. The commit before this one cited the
first of those as evidence the sim fires the right chunk. It was not evidence. The disassembly is.

The disc-wide figures in the previous commit — 112 rotators, 46 steps, 47 turned — were measured on the zone
chunk and so describe data no console ever executes. The live figures are the 26/17/13 above.

`render` grew an optional rotation-tick argument so a rotator can be looked at rather than counted: it builds
the map's rotators from COMMON's script, drives them, frames the one that turns furthest and renders it. A negative count builds
and frames without turning, which is the "before" of a pair taken from one camera — zero could not serve,
because zero also means "no rotators" and frames the whole zone instead. Two things that pass a count and
fail an eye: a **SNAP rotator turns exactly 2048 of 4096 about its own centre**, and a symmetric brush at 180°
is byte-for-byte the frame it started as; and **one call buys one step**, so a single tap turns an ACCUM
rotator by one speed's worth and stops. The render re-triggers each tick, which is a script holding the
rotation on.

---

## The damage function does not post to the AI, and a corpse now falls over

`q2_actor.ai_owned` carried this: *"a creature with a module posts rather than subtracts: 0x800584B4 hands the
amount to 0x800627F8 and jumps past the health store"*. It does not. **0x800627F8 is T_Damage**, with id's own
argument order and id's own content, and it subtracts health itself at 0x80062958 into `(entity+0x24)+0x108`.
The call at 0x800584B4 passes a DIFFERENT entity — the one at `entity+0x2EC` — and the caller has already
stored its own target's health at 0x800583F8. There is no posting anywhere. The flag was never set true, so
deleting it changed no behaviour; what it had been hiding was three things T_Damage really does, now
transcribed:

- **the surprise bonus** (0x800628C8-0x80062910): `svflags & SVF_MONSTER`, attacker has a client block,
  `!targ->enemy`, `targ->health > 0` → damage doubles. Four conditions, id's exactly. The first shot on a
  creature that has not noticed you is worth two.
- **FL_NO_KNOCKBACK** (0x800, 0x8006291C) zeroes the impulse and nothing else; **FL_GODMODE** (0x10,
  0x8006292C) zeroes the damage.
- **the corpse floor** (0x800629B4): health stops at -9999 however hard a body is hit.

T_Damage ends by calling the entity's own `die` at `entity+0xA4` (0x80062A9C) or its `pain` at `entity+0xA0`
(0x80062AF4). Neither had a caller here: `pain` and `die` are decoded into every creature's callback table and
nothing ever dispatched them. What can be reconstructed from a module's DATA rather than its code is the
animation, and that is now wired — every one of the seven modules names a death move (`Death1`, `Death 4`,
`St Death`, `Death 2`, `Death`, `Death3`, `Death2`), matched by frame range, so a kill installs it.

**`q2_monster_set_tick` skipped anything with `dead` set**, which is why this mattered. The body was still
drawn — the draw loop only checks `in_use` — so a killed Soldier stood in whatever pose the shot caught it in,
mid-stride, for the rest of the level. A corpse now runs the frame driver and not the AI: it animates, it does
not think, and it does not walk.

- [x] 51. **ANSWERED the same day: a move SELECTS a clip; the frame indexes into it.**
      The port walked the whole clip chain treating an AI frame as a position on one continuous timeline.
      That is what `0x8006B924` does when a position OVERRUNS its clip — but the engine holds a *current*
      clip pointer at `model+0x34` and only walks when it has to, so the clip is chosen elsewhere and the
      position is relative to it. What chooses it is not in the module's data, and the correspondence that
      is: **a move's frame count times three is exactly some clip's length, for 93 of the disc's 97 moves.**
      Per module: Tankcomm 16/16, Insane 18/18, Gunner 13/13, Infantry 11/11, Berserk 10/10, Soldier 16/18
      (lengths 12 and 33 have no clip), Arachner 9/11. Where several clips share a length the k-th move of
      that length takes the k-th clip, which reproduces the one correspondence already known from the other
      direction: the Soldier's consecutive moves 50-54, 55-61, 62-79 and 80-96 resolve to clips 1, 2, 3 and 4.
      Its death move 308-342 — 35 frames, 105 ticks — resolves to clip 11, which is 105 frames long and is
      the death animation, standing to fallen. The timeline walk put frame 308 at tick 924, inside clip 12,
      which is why a body stood up halfway through dying. The four moves with no matching clip fall back to
      the old walk rather than to nothing. `q2_model_anim_by_length` is the selector.

      The original evidence for `Q2_CRE_TICKS_PER_FRAME 3` stands and is now better supported: it was
      measured on four short consecutive moves, and 93 of 97 across seven modules agree with it.

- [x] 51a. **ANSWERED, and it corrects question 51's mechanism. There is no "current clip" to select.**

      `0x8006B924` reads `model->[0x34]` — which `model.h` already lists as `ofs_block_c`, the head of the
      clip chain — copies it to a STACK LOCAL at `16(sp)`, and walks that: `pos -= clip->frames`, advance,
      repeat while the position is past the end. It never writes back. At `0x8006B9C4` it reads
      `model->[0x34]` fresh again. So the pointer is the chain HEAD, read anew every call, and the walk is
      purely local.

      **The engine really does treat the position as an offset from the start of one continuous timeline**,
      exactly as `q2_model_anim_at` does. Question 51 concluded the opposite — "a move SELECTS a clip; the
      frame indexes into it" — on the strength of a `model+0x34` that turned out to be a file offset rather
      than mutable state, and that conclusion is withdrawn.

      What the length-matching selector actually fixed, then, is not the mechanism but the POSITION. If the
      engine walks from the head with `entity+0x100`, and `frame * Q2_CRE_TICKS_PER_FRAME` lands the
      Soldier's death move in clip 12 while clip 11 is the death animation, then **`entity+0x100` is not
      derived from the AI frame at all** — it is its own counter, advanced by something else.

- [x] 51b. **What advances `entity+0x100`? — the frame picks the position, at 30 units per frame.**
      `0x8007E9DC` is the function, and it reads end to end. `a0` is the entity, `a1` an enable flag:

          8007E9E0  lw   v0, 748(t1)     ; v0 = entity+0x2EC, the linked object
          8007E9E8  lw   t0, 40(v0)      ; t0 = its +0x28, a table (null -> return)
          8007EA00  lh   v0, 0(t0)       ; record[0], signed
          8007EA08  bltz v0, ...         ; negative record[0] ends the table
          8007EA1C  lh   a1, 56(v0)      ; a1 = (entity+0x2EC)->0x38, the CURRENT FRAME
          8007EA24  slt  v0, a1, v1      ; frame <  record.first -> next record
          8007EA38  slt  v0, v0, a1      ; frame >  record.last  -> next record
          8007EA40  subu v0, a1, v1      ; v0 = frame - record.first
          8007EA44  sll/subu/sll         ; v1 = ((v0<<4)-v0)<<1 = 30 * v0
          8007EA54  lhu  v0, 16(a3)      ; record+18 = the move's BASE position
          8007EA5C  addu v0, v0, v1      ; target = base + 30 * (frame - first)

      So the record is `{u16 first; u16 last; ...; u16 base}` in 20 bytes — the same table this project
      already reads as move names — and it is searched by **containment**: the move is the one whose
      `[first..last]` spans the current frame. The answer to 51 is therefore neither "a move selects a clip"
      (withdrawn) nor a free-running timer: **the position is a pure function of the frame**, linear, with a
      per-move base and a stride of 30.

      The rest of the function is bookkeeping that names three more fields:

          8007EA58  srl  a0, a0, 17      ; CURRENT position = entity+0xB0 >> 17
          8007EA6C  bne  a0, v0, ...     ; if it differs from target...
          8007EA8C  sw   v0, 268(t1)     ; ...entity+0x10C bit 0x40 is cleared, else set
          8007EB18  sh   t2, 256(t1)     ; entity+0x100 = the position
          8007EB48  sh   t2, 46(a0)      ; and (entity+0x2EC)+0x2E takes the same value

      `entity+0xB0` packs the live position in bits 17 and up; `+0x100` and the linked object's `+0x2E` are
      copies; `+0x10C` bit 0x40 is an **on-frame flag** — set exactly when the position equals what the frame
      says it should be.

      **The port's stride is 10 (`Q2_MODEL_TICKS_PER_FRAME`, `src/formats/model.h:332`); the disc's is 30.**
      Before changing it, the units have to be shown to be the same space — the port's 10 divides a tick to
      index a 4-byte clip entry, while the disc's 30 accumulates into a 15-bit packed field. The ratio is
      exactly 3 and that is suspicious, but "suspicious" is not "measured", and this project has been wrong
      before by reasoning where it could have counted. **Open follow-up: 51c, are the two strides in the same
      units?**

- [x] 51c. **Are the two strides in the same units? Yes — and that means 10 and 30 were never rivals.**
      One instruction settles it. The pose selector `0x8006B924`, which walks the clip chain, starts with:

          8006B95C  lh   s1, 256(s6)   ; s1 = entity+0x100
          8006B960  lh   v0, 0(v1)     ; v0 = clip->frames, the duration
          8006B968  slt  v0, s1, v0    ; while position >= duration...
          8006B984  jal  0x80070188    ; ...advance the clip pointer by its `next`
          8006B988  subu s1, s1, v1    ; ...and subtract that clip's duration

      It reads `entity+0x100` — **the exact halfword `0x8007E9DC` writes**. The position the frame computes
      is the position the clip walk consumes. Same field, same space, measured rather than argued.

      So the two constants are not competing calibrations of one quantity, and changing 10 to 30 would have
      been wrong:

        * **10 is position-units per CLIP frame.** `src/formats/model.c:541` uses it as `tick / 10 * 4` to
          index a 4-byte clip entry, matching the loader's documented multiply of clip durations by 10.
        * **30 is position-units per MOVE frame** — `base + 30 * (frame - first)` out of a block-D record.

      If the load-time multiply by 10 holds, one move frame spans exactly three clip frames. **The port's
      constant is not wrong; the port is missing a step.** It substitutes `frame * 10` for the block-D
      lookup, so it never applies a move's `base` and never gets the 3:1 relation — which is why animation
      plays but need not line up with the move the AI actually selected.

      The fix is therefore not a constant edit but an implementation of the block-D containment lookup. That
      is the shape of the remaining work, and it is larger than the one-line change it looked like.

      **Not verified:** the load-time multiply by 10 (and the by-5 on block D's `+12/+14/+16`) rests on the
      note at `src/formats/model.h:165-170`, not on disassembly. The `x*5` / `x*10` shift-add idioms do not
      appear in `0x80064780`+460 instructions, where that note points. The tool reads a raw on-disc duration
      of 1 for BASE1's model 0, which is consistent with a multiply at load — but consistent is not shown.
      **Opened as 51d: find and read the multiply, or retract it.**

- [x] 51d. **The position is in TENTHS of an animation frame — proven by a division, not by the note.**
      Disassembling the whole EXE (158,000 instructions, 0.4s) and matching the shift-add multiply idiom
      *strictly* — shift source and addend the same register, which array indexing never satisfies — leaves
      37 `x10` sites out of a first pass of thousands. Five sit immediately before the pose selector, and the
      first one reads:

          8006B5D8  lui  v0, 0x6666
          8006B5E4  ori  v0, v0, 0x6667  ; 0x66666667
          8006B5E8  mult t1, v0
          8006B5F0  mfhi t2
          8006B5F4  sra  v1, t2, 2
          8006B5F8  subu v1, v1, v0      ; v1 = t1 / 10
          8006B5FC  sll  v0, v1, 2
          8006B600  addu v0, v0, v1
          8006B604  sll  v0, v0, 1       ; v0 = (t1/10) * 10
          8006B608  subu fp, t1, v0      ; fp = t1 % 10

      `0x66666667` with `sra 2` is the textbook signed divide-by-ten. So the `x10` here is not a multiply at
      all — it is the **reconstruction step of a division**, and what it yields is a quotient in `v1` and a
      remainder in `fp`. The engine divides the animation position by 10 to get a frame index and keeps the
      remainder; `fp` is the sub-frame **interpolation fraction**, which is why the neighbouring code at
      `0x8006B640` folds it back in per part.

      **This proves the unit independently of the note**: the position is in tenths of an animation frame.
      That is what 51c actually needed, and it now rests on an instruction rather than a comment. It also
      confirms the 3:1 relation outright — 30 position-units per move frame over 10 per animation frame is
      **three animation frames per move frame**.

      **What is NOT retracted, and NOT confirmed:** the note's separate claims that block B's entries and
      block D's `+12/+14/+16` are scaled at load. Those are about different fields, they do not conflict with
      a tenths-unit position, and 140 strict `x5` sites remain unexamined. The specific claim that a *loader*
      multiplies clip durations still has no disassembly behind it — but nothing now depends on it.

- [x] 51e. **Block D read off the bytes: a NAMED move table, and it is not the table the engine walks.**
      Going to implement the containment lookup, I wrote the parser from `0x8007E9DC`'s offsets — `first` at
      +0, `last` at +2, `base` at +18 — and the dump came out as nonsense: "frames 25924..29793", every base
      1, `last` below `first` on a third of the records. `25924` is `0x6544`. **It was ASCII.** The hex says:

          44 65 61 74 68 31 00 00 00 00 00 00 00 00 D6 00 00 00 01 00  |Death1......|
          50 61 69 6E 31 00 00 00 00 00 00 00 D8 00 F4 00 D8 00 01 00  |Pain1.......|
          41 74 74 61 63 6B 34 00 00 00 00 00 F2 01 14 02 14 02 01 00  |Attack4.....|

      A record is `{char name[12]; u16 start; u16 end; u16 rest; u16 one}`, and the moves **tile one
      continuous timeline**: 0..214, 216..244, 246..286, 288..394, 396..496, 498..532, 534..850, ...

      Censused over 14 zones, 92 moves: **every span even, every gap exactly 2, `rest` = `end` on 77 and
      `start` on 15, `one` = 1 on 90** and 0 on exactly two — both a mover's move named "Move", so `one` is
      a flag rather than the constant it looked like at n=31.

      ~~**The even spans verify the load-time multiply by 5.**~~ **Withdrawn the same session — see 51f.**
      The reasoning was that a move's frame count is `value * 5 / 10` = `span / 2`, integral only because
      every span is even. The arithmetic is fine; the multiply it rests on is not there. The even spans stay
      as a measured and still-unexplained regularity.

      **And block D is NOT the table `0x8007E9DC` walks.** That walk terminates on a *signed* halfword at +0;
      block D's +0 is a name. So the runtime table is built from block D at load — different layout, scaled
      by 5 — and the containment lookup cannot be implemented against the file format directly. That is why
      this round ships the reader and the census but wires nothing into the animation path: the missing piece
      is the load-time transform, not the lookup.

      **Open as 51f: find the loader that builds the runtime move table from block D.** It should sit near
      the 140 strict `x5` sites, which are now the obvious place to look.

- [x] 51f. **There is no load-time multiply by 5. The `x5` idiom is a 20-BYTE RECORD STRIDE.**
      Following 51e's own pointer to the `x5` sites finds the one that writes a move record's `+12` and
      `+14` — and it is not scaling anything:

          80066954  sll  v0, v1, 2     ; index * 4
          80066958  addu v0, v0, v1    ; index * 5
          8006695C  sll  v0, v0, 2     ; index * 20   <-- a SECOND shift
          80066960  addu v0, v0, s0
          80066968  addu v0, v0, s3    ; &record[index]
          8006696C  sh   v1, 12(v0)    ; start = an unscaled value from a0+158
          80066990  sh   t2, 14(v0)    ; end   = 45, likewise unscaled

      `sll 2; addu; sll 2` is `x5` then `x4`, which is **`x20` — this table's own record size**. Classifying
      every strict `sll rX,rY,2; addu rX,rX,rY` in the EXE by the shift that follows it settles the shape of
      the whole population:

          x20 = 44     x80 = 13     x40 = 11     x10 = 34     bare x5 = 62

      So the `x5` shape is dominantly address arithmetic for 20-, 40- and 80-byte records, and `x10` is the
      tenths-of-a-frame divide from 51d. **No site scales `+12/+14/+16` by 5.** The note at the head of
      `model.h` claimed one; it is now marked as unsupported there, and 51e's inference from the even spans
      is withdrawn in place.

      What survives untouched, because each was read off bytes or instructions rather than inferred:
      block D's layout, the tiling with gap 2, the 92-move census, the tenths unit, and the fact that block D
      is not the table `0x8007E9DC` walks. What is now open is narrower and better posed than 51f was:
      **51g — what units are block D's `start`/`end` in, given no scale factor exists, and why is every span
      even?** `0x80066954` is the place to resume: it builds records, and whatever feeds `a0+158` is the
      answer.

- [~] 51h. **The scale between block D and the animation position is FIVE after all, and a
      constant in the weapon code is what says so.** — *no loader found; the inference is forced.*
      51f is not withdrawn: its sweep is right, there is no load-time multiply, and `0x8006D330` walks
      block D itself (a 20-byte stride off `model+0x38`, names compared three words at a time), so the
      `start` and `end` a caller gets are the disc's own numbers.
      What 51f could not see is a caller that pins the two systems together. `0x80050454` is one:
      it stores a move record's `+12` straight into **viewmodel+256**, which is `entity+0x100` — the
      halfword the pose selector at `0x8006B924` consumes — and ends the move when the position walks
      past `+14`. Five weapons reach it, each naming one move (`Fire`, `Set`, `Spin`), and the model
      banks carry exactly those names on exactly those models.
      Taken literally that makes the two unit systems the same, which contradicts 51d and 51g. **One
      constant rules the literal reading out.** `0x800503F8` refuses to prime the hand grenade until
      viewmodel+256 reaches **380**, and `HandGren G`'s `Set` move runs **0..94**. Unscaled the position
      tops out at 94, the test can never pass, and a grenade can never be cooked — which the console
      plainly does. The threshold must land inside the span, so the scale is at least 4.05; five is the
      ratio 51d and 51g already established (two per frame against ten), and at five the span is 0..470
      with the prime four fifths of the way through the arm coming up.
      The port applies the five in `src/game/viewweapon.c::anim_step` and says at the call site that the
      instruction for it has not been found. **What is still open is where the five happens on the
      console** — a patch of the loaded block D, a scaled copy, or a second position field. `0x80066954`
      writes a record's `+12`/`+14` on a 20-byte stride and is the nearest thing seen so far, but it
      writes a creature's numbers, not a scale.

- [x] 51g. **Two units per animation frame — and move `i` IS clip `i`.**
      Answered by comparison rather than by more disassembly. BASE1 model 15 carries 31 clips and 31 moves;
      laying the lists side by side, in order:

          clip lengths : 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 15 9 9 9 15 21 ...
          span / 2 + 1 : 108 15 21 54 51 18 159 72 30 90 117 105 135 30 42 30 69 3 15 9 9 9 15 21 ...

      Censused across 25 zones over every model carrying both a move table and a clip chain: **34 models,
      0 mismatches.** So block D counts **2 units per animation frame**, a move of span S is `S/2 + 1`
      frames, the two-unit gap between moves is exactly one frame, and **the k-th move drives the k-th clip**.

      That also recovers the factor of 5 arithmetically, with no load site needed: block D counts 2 per
      frame, the position counts 10 per frame, and 2 * 5 = 10. **The old note was right about the ratio and
      wrong about the mechanism**, which is exactly why it survived three rounds of checking — 51f could
      refute the multiply without touching the ratio it implied.

      Shipped, because this one is finally implementable: `q2_model_clip_for_move()` maps a move to its clip
      **by index**, replacing `q2_model_anim_by_length()`'s match-on-length-with-a-`skip`-to-break-ties. An
      index cannot pick the wrong one of two equal-length moves; a length can, and the Soldier has four
      30-frame clips. It returns false rather than posing from a clip whose length disagrees, so a model
      outside the rule reports instead of animating wrongly. `tests/model` pins the unit.

      **Still open — 51h: the AI frame timeline is not block D's.** A creature module numbers its moves in
      its own global frame space (the Soldier's run 0-11, 12-29, ..., 441-464, top frame 474) and has 18
      moves where its model has 31. Block D's ranges halve to 0-107, 108-122, 123-143, ... which is a
      different numbering. Joining the two — by name, by order, or by something else — is the last link
      between what the AI selects and what the model plays.

- [x] 51h. **Three model frames per AI frame — 96 of 101 moves, across every creature on the disc.**
      BASE1 model 15 is the Soldier itself, so its 31 moves and the module's 18 can be compared directly.
      Multiplying each AI move's length by 3 and asking whether the model has a clip of exactly that length:

          AI 176-214 (39) -> 117 = clip 10      AI  50-54  (5) ->  15 = clip 1
          AI 146-175 (30) ->  90 = clip 9       AI  55-61  (7) ->  21 = clip 2
          AI 256-265 (10) ->  30 = clip 8       AI  62-79 (18) ->  54 = clip 3
          AI  97-98   (2) ->   6 = clip 25      AI  80-96 (17) ->  51 = clip 4
          AI  99-104  (6) ->  18 = clip 5       AI 272-307(36) -> 108 = clip 0
          AI  12-29  (18) ->  54 = clip 3       AI 308-342(35) -> 105 = clip 11
          AI  39-44   (6) ->  18 = clip 5       AI 441-464(24) ->  72 = clip 7
          AI 109-122 (14) ->  42 = clip 14      AI 465-474(10) ->  30 = clip 8

      **16 of 18.** The factor of three is not a new constant — it is `30 / 10`, the position units per AI
      frame over the position units per model frame, so 51b and 51d already implied it and this is the first
      time it has been checked against data. It also independently confirms the existing
      `Q2_CRE_TICKS_PER_FRAME = 3`.

      **The two that fail are the interesting part**, and they are recorded rather than explained away:
      AI 215-247 (33 frames, wants a 99-frame clip) and AI 0-11 (12 frames, wants 36). The Soldier's model
      has neither length. Its three classes (87, 89, 88) share a single model — BASE1 has exactly one
      Soldier — so a variant model does not account for them. Either those two moves span more than one clip,
      or the range for them is not a plain inclusive span.

      ~~Only the Soldier could be tested.~~ **Wrong, and backwards.** `cmd_model` reads a map's COMMON.DAT
      only; the creature models are in its **ZONE** banks. Teaching the command to fall back to
      `ZONE0..7.DAT` reaches all seven creatures at once, and the sample goes from 18 moves to 101:

          Soldier    16 of 18      Gunner     14 of 14
          Tankcomm   16 of 16      Infantry   12 of 12
          Insane     19 of 19      Berserk    10 of 10
          Arachner    9 of 12
          ----------------------------------------------
                              96 of 101

      **96 of 101 AI moves have a clip of exactly three times their frame count.** That is the rule, across
      every creature the game ships, and it settles the factor of three as data rather than as the
      arithmetic consequence of `30 / 10` it started as.

      The five exceptions, named rather than smoothed over:

          Soldier   215-247   33 frames, wants 99   -- no clip of 99, and none of 33 either
          Soldier     0-11    12 frames, wants 36   -- no clip of 36; there IS a 12 (clip 26)
          Arachner   16-24     9 frames, wants 27   -- no clip of 27; there ARE three 9s
          Arachner   16-24     9 frames, wants 27      (the module lists this range twice)
          Arachner   25-33     9 frames, wants 27   -- likewise

      **How strong is 96 of 101? Quantified, because a hit rate means nothing without its null.** For each
      creature, the chance that an arbitrary multiple of three lands on one of its clip lengths:

          Soldier   16/18 ( 89%)   chance 35.8%      Gunner    14/14 (100%)   chance 25.0%
          Tankcomm  16/16 (100%)   chance 31.6%      Infantry  12/12 (100%)   chance 20.4%
          Insane    19/19 (100%)   chance 35.0%      Berserk   10/10 (100%)   chance 45.0%
          Arachner   9/12 ( 75%)   chance 40.0%
          -------------------------------------------------------------------------------
          TOTAL     96/101 (95%)   expected by chance 33.4 (33%)

      **95% observed against 33% expected**, with five of the seven creatures perfect. The rule is not a
      coincidence of a dense clip list.

      **A tempting explanation for the five, tested and REJECTED.** Since a model's clips are one continuous
      timeline, an exception could be a move spanning two clips rather than one — and indeed every one of the
      five has a run of consecutive clips summing to exactly what it wants (Soldier's 99 = 30 + 69, Arachner's
      27 = 9 + 18). That evidence is worthless: on the Soldier's chain **89% of arbitrary multiples of three**
      have such a run, and 62% on the Arachner's. A test that almost everything passes distinguishes nothing.
      The consecutive-clip story is dropped, and the five stay unexplained rather than explained badly.

      **Where the five actually live: they are an IDENTIFICATION gap, not a rule gap.** Splitting the 101
      moves by whether the module decoder could put a block-D name to them:

          moves the namer RESOLVED : 88 hit, 2 miss  (98%)
          moves it did NOT         :  8 hit, 3 miss  (73%)

      And the five, named individually:

          Soldier   215-247   33 frames wants  99  (unnamed)
          Soldier     0-11    12 frames wants  36  (unnamed)
          Arachner   16-24     9 frames wants  27  "Sway"
          Arachner   16-24     9 frames wants  27  "Sway"   <- the module lists it twice
          Arachner   25-33     9 frames wants  27  (unnamed)

      The two Arachner "Sway" rows are one move counted twice, so there are **four distinct exceptions, and
      three of them are moves whose identity is unresolved**. Exactly **one identified move on the whole
      disc violates the 3:1 rule — the Arachner's "Sway"**.

      That is a much narrower statement than "five moves do not fit": for every move the decoder can name,
      the rule holds 88 times out of 89. The residue is concentrated where the decoder cannot say what the
      move is, which is a different open question — and a cheaper one, since naming a move needs no new
      theory of animation.

      Four of the five have a clip at exactly **1x** their length instead of 3x. That is a suggestive
      pattern and it is deliberately NOT promoted to a rule here: it was noticed after the fact, from five
      cases, and a fallback ratio chosen because it makes the leftovers fit is the precise move this file has
      had to withdraw twice already. It is recorded as an observation. The one case that fits neither ratio
      is the Soldier's 215-247.

- [x] ~~51. **The AI frame → model clip mapping drifts across a long move.**~~ `Q2_CRE_TICKS_PER_FRAME` is 3 and
      it lands the START of the Soldier's `Death1` correctly: posed at AI frames 310, 314, 318 and 322 the
      model is a body collapsing to the floor, progressively. But the move runs 308-342, and at 330 and 336
      the same creature is standing upright again — the timeline has walked into the next clip well before
      the move ends. Either the move's frame range and the model's clip lengths do not correspond one to one
      for this creature, or the tick scale is not a constant 3 across the whole timeline. The measurement that
      established 3 used the first four moves, which are consecutive and short; nothing checked a move 35
      frames long. `q2psx-inspect mob <disc> <map> <zone> <n> <out.ppm> <ai-frame>` is what took those poses.

## The multiplayer runtime had no caller anywhere in the game

`multiplayer.[ch]` reconstructs the whole of QMULTI.C — the scoring, the frag and time limits, the VERSUS
round rules, the spawn selector, the banner countdown, the attribution rule and the two game-state requests —
and the test suite was the only thing that had ever run any of it. Nine of its entry points had no caller
outside the module: `q2_mp_session_init`, `q2_mp_player_killed`, `q2_mp_frame`, `q2_mp_take_request`,
`q2_mp_banner`, `q2_mp_may_respawn`, `q2_mp_attribute_kill`, `q2_mp_find_winner`, `q2_mp_hud_image`. The
rules were right and nothing ever asked them anything.

The client now runs a match. `--dm` boots an arena, `--dm-mode`, `--dm-players`, `--dm-frags` and
`--dm-minutes` set it up, and what runs is the reconstruction rather than a re-statement of it:

- **the spawn**: the local player starts at a `MultiSpawn`, picked by `q2_mp_select_spawn` — farthest from
  everybody already placed, ties broken by an RNG. All five arenas tried resolve their points: MATRIX1 4,
  MATRIX5 8, THEVAT 5, PODCITY 8, FRAGTOWE 4.
- **the clock**: `q2_mp_frame` on the sim's own 1/300 s step. A one-minute match ends at 18010 dt — the
  limit is `level_time > minutes * 18000` and 18000 units is sixty seconds — with the banner `TIME UP`,
  then request 11, `load MPResults`, winner 8, `DRAWN MATCH`, scoreboard title `DM SCORES`, HUD set
  `qk2_menu.lbm` for two players.
- **the death**: a killed player goes to `q2_mp_attribute_kill` with the means of death and then to
  `q2_mp_player_killed`, before the death screen opens.
- **the cut modes**: asking for CTF prints that the mode is cut and the front end cannot select it, rather
  than pretending it is a shipped feature.

Taking the request also STOPS the session, which is not a detail: on the console the request changes the game
state and the level hook stops running. The first version left it ticking and the runtime re-asked on every
frame — sixty-odd identical requests for one match that ended once.

- [x] 52. **ANSWERED: the client was asking for the single-player icon sheet on maps that carry only the
      multiplayer ones, and the failure was silent.** `q2_menu_icons_name` picks `qk_menu.lbm` in single
      player, `qk2_menu.lbm` for a two-player match and `qkm_menu.lbm` for three or four — which is the same
      thing `q2_mp_hud_image` says, and neither had a caller that knew whether a match was running. The
      client passed `menu.multiplayer, 1` — the MENU's flag, which is about which menu pages to show, not
      whether a game is in progress — so every arena asked for `qk_menu.lbm`, and no arena carries it.

      Silent because `q2_menu_font_upload` returns Q2_OK when ANY of its images lands, and the two text
      atlases always do. `icons_resident` was false, `tpage_icons` addressed an empty texture page, and the
      status bar dutifully emitted its quads into it. The draw is now gated on `icons_resident` rather than
      on the upload having half-succeeded, and a map that lacks the sheet says which one it lacks.

      Surveyed across the thirteen arenas: **none of them carries `qk_menu.lbm`**, which is right — an arena
      is never played in single player — and all thirteen carry `qkm_menu.lbm`. Twelve carry `qk2_menu.lbm`;
      **FRAGTOWE does not**, so a two-player match there has no status bar and a three- or four-player one
      does. That is a fact about the disc, not about this port. No single-player map regressed: eight checked
      by hand and BASE0's bar is pixel-identical before and after.

  *As first written, before it was chased:*

- [x] ~~52. **No HUD is drawn on any arena map.**~~ Found by looking at a deathmatch instead of reasoning about
      one. On BASE0 the overlay draws health, armour and ammo; on MATRIX5 it draws nothing, and it is the MAP
      and not the session — the same map without `--dm` is equally blank. The gate is not the cause: at frame
      60 on both maps `hud_ready 1, font 1, menu 0, mission 0, mcard 0`, so `q2_hud_build_ot` runs and emits
      nothing visible. The font upload reports no failure either. The suspicion is VRAM: the HUD atlas is
      uploaded into the map's own texture memory and an arena's pages may land on top of it, which would make
      this a load-order fault rather than a HUD fault. Not chased yet.

## The scoreboard the runtime was asking for

`Q2_MP_REQ_RESULTS` is engine state 11, "load MPResults", and until now the client recorded the request and
did nothing with it. QMRESULT is a level directory of its own — an 840-byte zone, a 99 KB COMMON and a 29,988
byte LevelBin — and that module carries every word the screen shows: the six titles in mode order, the four
colour names, `"%s TEAM SCORED %d"`, and the prompt `ALL PLAYERS PRESS` / `FIRE TO CONTINUE`.

`q2_mp_scoreboard` composes those lines from the session. Deathmatch gives the title, a line per player, and
the prompt; a team mode inserts the team lines after the players, one for each team with a score; VERSUS
prints ROUNDS WON rather than frags, because that is what its `team_frags` array holds. Nothing in the text
is invented — the strings are the module's, read out of it.

**The layout is not reconstructed and is marked as such.** Where QMRESULT puts each line goes through engine
text calls whose offsets have not been read, so the port stacks them centred. Showing the right words in the
right order is what it claims; the original's pixels are not.

Two mistakes on the way, both worth keeping because both are easy to repeat:

- **A line's position is the CONTEXT's home, not the pen's x and y.** The pen carries state across a string;
  `ctx->home_x`/`home_y` say where a string starts. Setting the pen put all five lines on one row, each
  beginning where the last one ended — and it looked like a layout bug rather than an API mistake.
- **`q2_hud_measure` returns CHARACTERS, not pixels.** It is the original's measurer at 0x800702A0,
  off-by-one and all, and the glyph advance is a constant 8. Centring on its raw value put the block half a
  screen right.

## The split screen, and what is behind each viewport

The screen module has had every layout decoded for a long time — `ONE`, `TWO_H`, `TWO_V`, `QUAD`, with each
viewport's origin, size, GTE projection, geometry offset, far plane and status-bar anchor — and the client
installed `ONE` and left the rest to an F5 debug cycle whose own comment said the extra viewports show the
same camera because there is one player. The layout is not a debug toy: it is chosen by the session's PLAYER
COUNT at 0x800B3356, through the jump table at 0x800AC90C. One or none is full screen, two is a split whose
axis is the HORIZONTAL SPLIT setting, three is the quad layout with the view count forced to three
(0x8003FAE4), and four is the quad. `--dm-players` now installs it.

Each viewport also looks from its own place. Every player is put through `q2_mp_select_spawn` in turn, each
placed against the ones already placed — which is what that selector is *for*: it takes the point farthest
from everybody standing somewhere, so four players spread across an arena instead of piling onto whichever
point comes first. On MATRIX5 with four players they land on MultiSpawn 5, 3, 6 and 1 of the eight.

**What each viewport is NOT is a second player.** There is one `q2_sim`. Viewport 0 is the simulated player;
1 to 3 stand at their spawn and do not move, and their status bars read player 0's health and ammo because
the status bar is fed from the one inventory that exists. The screen work, the layout selection, the spawn
spread and the per-viewport HUD set are real; the other three participants are not, and the code says so
where it does it rather than in a note here.

- [x] 53. **DONE: four players in one world.**

      The tick was two things wearing one name — the player's frame (movement, view, weapon, the volumes they
      stand in) and the world's (the entity sweep, the effects, the glint, the clock). With one player they
      are indistinguishable. With four they must not be: running the entity sweep four times would age every
      item respawn four times as fast and step the effects four times a frame.

      So the world half is gated on `cur_player == 0` and `q2_sim_advance_player` runs an extra player's frame
      against the world that `q2_sim_advance` has already advanced this frame. The entity world has taken a
      player INDEX since it was written and nothing had ever passed anything but 0; every player publishes
      their position through it now, so they are all visible to the same item and trigger logic.

      One bug caught by reading the numbers rather than the render: the extra players were seeded by copying
      player 0's state and overwriting the position. A `q2_player` carries its collision NODE, and a node is
      where you are — so a player placed elsewhere with someone else's node fell out of the world. Two of four
      ended a capture at y 64847. Spawning each through `q2_sim_spawn` with `cur_player` set fixes it: four
      players at y -1024, 604, 0 and 604, all on the floor.

      **Single player is byte-identical** to before the whole refactor, all 26 tests pass, and the four-way
      split renders four viewpoints of one MATRIX5.

      **The combat block is split too.** `rules`, `rng`, `projectiles`, `targets` and `target_count` are the
      world's — one list of bolts in flight, one set of things that can be hurt — and the inventory, weapon,
      refire gate, view kick, chaingun spin, hurt-actor and last-shot record are a player's. Four players now
      have four of each: `100 hp` apiece on MATRIX5.

      They are SWAPPED in and out of `sim->combat` around a player's tick rather than addressed through an
      index, because `sim->combat.inv` appears eighty-six times across the game, the client and the tests, and
      every one of those sites means "the player whose frame is running" — which is precisely what the swap
      makes true. `cur_player` selects which, exactly as it does for `player[]`. Four checks in `test_sim`
      hold it down: a player's health survives another's tick, a player's weapon survives another's tick,
      hurting one leaves the other alone, and three extra players do not advance the world clock.

      Single player is byte-identical, all 26 tests pass, and COMMAND still reports 22 fire calls, 22 sent.

  *As first written:*

- [x] ~~53. **Four simulated players — the client half is done, and `sim.c` now holds four players instead of
      one.**~~ The scaffolding is in: `q2_sim.player` is `q2_player[Q2_SIM_MAX_PLAYERS]` with a `cur_player`
      index, and every one of the 52 references in `sim.c`, 15 in `simcombat.c` and the rest across the tests
      and tools goes through it. `cur_player` stays 0 and nothing sets it yet, so this is a change of shape
      and not of behaviour — deliberately, because the way to verify a refactor of the file that owns the tick
      is to prove it changed nothing.

      **Verified byte-identical**: BASE0 in the client renders the same frame pixel for pixel before and
      after; COMMAND still reports 22 fire calls, 22 sent; the four-player deathmatch still places four
      players at four MultiSpawns; all 26 tests pass.

      One thing found on the way and worth recording, because it nearly went in silently: `q2_save` has a
      `player` member of its own, a single `q2_player` and not an array. A regex that turned `s->player` into
      `s->player[0]` hit it in `save.c` and in the inspector's `save` command, where it compiled in one place
      and not the other. The save format is untouched and the round-trip test — 156 checks over health,
      armour, ammo, tier, keys, weapons, the held weapon, the refire gate, the view kick, the chaingun spin
      and the weapon RNG — still passes.

      What remains is the tick: `q2_sim_tick` runs the player half and the world half together, so four
      players in one sim means splitting it and calling the world half once. The sections are already
      separable — `q2_fx_tick`, `q2_fx_timed_tick`, `q2_fx_glint_advance` and
      `q2_item_mega_health_tick` are world; everything from `update_view_offset` to `update_pain` is a
      player's; `update_triggers` and `q2_sim_combat_tick` are per-player and want the world's script and
      projectile list, which is exactly what one shared sim gives them.

  *As first written:*

- [x] ~~53. **Four simulated players — done in the client, still owed in `sim.c`.**~~ `q2_sim` is now an array
      indexed by player, and players 1..3 each get their own instance: spawned at their own MultiSpawn,
      advanced every frame on their own `q2_pad_state`, with each viewport following its own player's eye and
      view angles rather than a camera parked at a spawn point. Measured on MATRIX5 with four players after
      400 frames — positions `[-14926 -1024 12546]`, `[9412 42 3321]`, `[-24811 -2556 4128]`,
      `[-3204 611 17148]`, having moved 9029, 4239, 1923 and 6223 units from where they started, on three
      different floors. Single player is byte-identical to before the refactor.

      **What is still wrong is the world.** Each instance owns a copy of the map's items and its own script
      runtime, because the player lives inside `q2_sim`, and only player 0's is read or drawn. The duplicates
      are invisible and self-consistent, so this is a cost rather than a bug — but it means four players do
      not yet share a world, and therefore cannot pick up the same item or shoot each other. Fixing it is a
      change to `sim.c`: the world half and the player half want separating, and every rate, timer and event
      in that file has to end up on the right side of the line. That is the remaining piece, and it is the
      one that turns four people walking about into a match.

  *As first written:*

- [x] ~~53. **Four simulated players.** The remaining piece is `q2_sim` being an array rather than a member:~~
      four players means four movement states, four inventories, four view weapons and four sets of pad
      input, and every consumer in the client that says `c->sim` today means `c->sim[p]`. Nothing found so far
      says the sim cannot be instanced — `q2_sim_init` already takes the zone as an argument, so several can
      share one world — but it is a refactor of the client rather than a reconstruction of the original, and
      it is the largest single piece of multiplayer left.

## Naming the import slots, and why six creatures never fired

A decoded creature reaches the engine through its module's import table, and `cre_actions.c` ran every kind of
decoded step except one: `Q2_CRE_OP_CALL` fell through to `default: break`. 107 call steps across the disc did
nothing.

The import loader at `0x8007DA00` writes all 71 slots individually — `sw v0, N(s0)` with the address
materialised two instructions above — so any slot can be named by reading that one function. The method was
already checked against `+0x28` (`0x8006FC1C`, the SVECTOR rotate) and `+0xEC` (`0x80061118`, `fire_hit`); it
reproduces both. The rest of the slots the census reports:

| slot | address | what it is |
| --- | --- | --- |
| +0x84 | 0x80061DFC | a hitscan — goes through `0x80044C44`, the swept move |
| +0x98 | 0x8006210C | the rocket — `0x80062164` calls `0x8004AF28`, which combat.h already records as being called *from that address* |
| +0x8C | 0x8006217C | the rail — `0x800621A4` calls `0x8004917C`, named in combat.h |
| +0x80 | 0x80062000 | a bolt with a visual — calls `0x8004E920`, an effect constructor (effect.h) |
| +0xFC | 0x80062240 | three calls to one spawner (`0x800619E0`) — the shape of a spread |
| +0x1C | 0x8005C8C8 | unnamed |
| +0x2C | 0x8006C6C8 | unnamed |
| +0x38 | 0x8006CC44 | unnamed |
| +0x94 | 0x800614D4 | unnamed |
| +0xA0 | 0x80031094 | unnamed |
| +0xB4 | 0x8005EF84 | unnamed |
| +0x114 | 0x8005CBBC | unnamed |
| +0x12C | 0x80040800 | unnamed |

Three of those five confirm themselves against addresses this project had identified for other reasons
entirely, which is the strongest form the evidence takes here.

The five are now routed to the same fire hook a transcribed creature uses. **The four already named as vector
arithmetic (+0xC0, +0xC4, +0xC8, +0xD8) are deliberately not:** they are the muzzle maths every fire think
does first, and they are 40 of the 107 call steps. Treating them as shots would have every creature fire three
times an animation frame — which is exactly the mistake the Soldier's transcription notes warn about.

**What has NOT been observed is one of them firing in play.** Tankcomm's thinks 8, 10 and 13 carry +0x80,
+0x98 and +0x84, and every one is marked gated; a 400-frame `--watch` capture on COMMAND produces its sounds
and no shots, and the same capture produces the same ten shots with the routing removed, so those ten are
somebody else's. The creature is not reaching its attack thinks in a run that short. The routing is covered by
five checks in `test_creature` instead — hook called with an enemy, not called without one, not called at a
dead enemy, carrying the slot it came from, and not called for muzzle arithmetic.

## Every decoded creature has been doing nothing, and the counters found it in one run

`cre_actions.c` decodes what each of a module's think functions DOES and executes it. Six of the seven
modules on the disc run entirely on that. It had never executed anything.

`creworld.c` called `q2_creature_bind_thinks(&m->bind, ...)` and then `q2_creature_bind(&m->bind, ...)`, and
`q2_creature_bind` opens with `memset(b, 0, sizeof(*b))`. The think table was installed and wiped two lines
later, so `q2_cre_run_think` found `b->think` NULL and returned — every time, for every creature, on every map.

It was invisible because it looked exactly like the state it was meant to be an improvement on: a Tank
Commander that walks, chases and does nothing is what "no hand transcription yet" looks like. The file's own
header said as much, and had been stale since the trampolines were added.

Found by counting rather than reading. Adding `q2_cre_action_stats` — thinks run, thinks unbound, calls seen,
calls unclassified, fire calls, and where each fire call stopped — and running one 400-frame capture on
COMMAND printed `31 thinks (31 unbound)`, which is not a subtle number. Moving one line below the bind:

| | before | after |
| --- | --- | --- |
| thinks run / unbound | 31 / 31 | 31 / 0 |
| CALL steps reached | 0 | 31 |
| creature sounds | 3 | 34 |

WASTE3's Gunner goes from 0 decoded thinks to 22 and from silence to 16 sounds. BASE0 reports 0 decoded
thinks, which is correct: the Soldier is hand-transcribed and does not use this path.

The 31 calls COMMAND reaches are all import `+0x12C` (`0x80040800`), which is not one of the five projectile
spawners. It takes a player index or 4 for "all", reads the active player count at `0x800B2C2C`, walks the
player array at `0x800D5C30` and tests each player's position against a supplied point and a radius on the
stack — a per-player proximity call. Named that far and no further.

So the fire routing committed an hour ago is still not observed in play: what a Tank Commander reaches in a
400-frame capture is its sounds and this proximity call, not its attack thinks.

## A creature was being sent to a melee it does not have

Chasing "why does a decoded creature never fire", with counters rather than reading. `q2_ai_decision_stats`
records how far an attack got — checkattack reached, enemy invisible, decision run, decision yes, attack
callback run, attack callback missing — and `q2_cre_action_stats` gained a per-slot tally of which moves the
generic implementation could find. One capture on COMMAND said it:

    attacks  190 checkattack (77 blind, 60 decided, 53 yes), 10 attack calls
    moves    attack set 0 / missing 0, melee 0 / missing 43

Fifty-three attacks granted, and the generic attack handler never ran once — while the melee handler ran 43
times and found no move to play. The Tank Commander was going to melee, standing there, and going again.

**Its module has no melee callback.** The census lists stand, idle, walk, run, attack, sight, pain and die.
But `q2_creature_spawn` installed the IMPLEMENTATION's callbacks unconditionally, and the generic
implementation supplies a handler for every slot because it does not know which creature it is being used
for — so `m->melee` was non-NULL for a creature that has no melee. `M_CheckAttack` then does exactly what the
original does: `m->attack_state = m->melee ? Q2_AS_MELEE : Q2_AS_MISSILE`. The transcription was right; it was
being lied to about the creature.

A callback the module does not have now stays NULL: `c->callback[slot]` is the module's own address for that
slot and is zero when it has none, so the module decides and the implementation only supplies. After it, on
the same map: `attack set 1 / missing 0, melee 0 / 0` — no phantom melee, and the attack move installs.

The Soldier is unaffected, as it should be: its module has every callback, and BASE0 reports the same 19
shots before and after.

- [x] 54. **ANSWERED: the generic attack handler always takes the FIRST move its callback installs, and for
      the Tank Commander that is the one attack of four that does not shoot.**

      A think index only ever runs because an animation FRAME names it, so the question was never about the
      AI — it was about which frames the move being played carries. `q2psx-inspect creatures` now prints that
      for all 97 moves on the disc: `(via) range -> think bytes`, the callback that installed each move and
      the distinct think bytes its frames call.

      The Tank Commander's attack callback installs FOUR moves:

      | range | thinks its frames call |
      | --- | --- |
      | 77-114 | 0, 6, 11, 9 |
      | 168-196 | 0, **13** |
      | 115-135 | 0, 6 |
      | 55-70 | 0, **8** |

      Its fire thinks are 8, 10 and 13. `generic_attack` calls `set_via(m, 6)`, which returns the first move
      for that slot — 77-114, whose thinks are the `+0x12C` proximity call and two sounds. Every attack it
      makes is the one that does not shoot, which is exactly the 31 unclassified calls and no fire calls the
      counters reported. `cre_generic.c` documents "the first move a callback installs is the one taken" as a
      caveat about randomised animation; for this creature it silently disables firing altogether.

      The same census answers it for the others, and they do not all lose: **the Arachner's first attack move
      (94-109) carries thinks 3 and 4 and both call `+0x8C`, the rail**, and the **Infantry's first (199-206)
      carries think 11, which melees and calls `+0xFC`**. Those two should fire on the current code. The
      Gunner loses the same way the Tank Commander does — its first attack move (108-128) carries thinks 0
      and 1, while `+0x84` is on think 2. Berserk and Insane have no attack move at all, which is right: they
      are melee creatures.

- [~] 55. **READ, for the Tank Commander. It is a range-and-chance table, and the move the port plays is the
      one for a DEAD enemy.**

      `tankcomm_attack` is at `module+0x11BC`, and it decodes cleanly:

          v1 = self->enemy                     ; entity+0xBC
          v0 = v1->[0x24]->[0x108]             ; the enemy's health
          if (v0 < 0) {                        ; bgez v0, +0x54
              self->currentmove = M_77_114;    ; module+0x1DE4
              self->aiflags &= ~0x200;
              return;
          }
          v    = enemy->origin - self->origin  ; three subtractions onto the stack
          dist = import[+0xB8](v)              ; the vector's length
          r    = import[+0x14]()               ; 0..32767

          if (dist < 1501)       move = (r < 13106) ? M_168_196 : M_55_70;   ; 40%
          else if (dist < 3001)  move = (r < 16384) ? M_168_196 : M_55_70;   ; 50%
          else if (r < 10813)    move = M_168_196;                           ; 33%
          else if (r < 21626)  { move = M_115_135; self->[0xA8] = g + 50; }  ; 66%
          else                   move = M_55_70;

      **The first branch is the whole bug.** `M_77_114` — the move the port plays every time, whose thinks are
      a proximity call and two sounds — is what the creature does when its enemy's health has gone NEGATIVE.
      It is the after-the-kill animation. `q2_creature_move_via` returns the first move for a slot in decode
      order, and for this creature the first is the one that only ever runs over a corpse.

      The three live-enemy moves are the ones that shoot: `M_168_196` at close range or on a roll (nineteen
      consecutive frames of think 13 — the sustained burst), `M_55_70` otherwise (three separate think 8s),
      and `M_115_135` on the middle roll at long range, which also sets a timer at `entity+0xA8`.

      The 1501 and 3001 are the range bands in the module's own units, and 13106, 16384, 10813 and 21626 are
      0.4, 0.5, 0.33 and 0.66 of 32768 — the same shape as `M_CheckAttack`'s odds and as id's `tank_attack`.

      What is left is transcription: this is one of the six modules, read but not yet written, and the other
      five have their own tables at their own `cb at` addresses. The census prints those addresses, the thinks
      each move calls and where they sit, so each is now a reading job rather than a search.

  *As first written:*

- [x] ~~55. **Which of a callback's several moves the module picks.**~~ The module's attack function branches —
      on range, on a roll, or on state — and the decoder records only WHICH moves a callback installs, not
      the branch that chooses between them. Four attack moves for the Tank Commander, two for the Gunner,
      two for the Infantry. Until that branch is read, a generic creature will always play one of them, and
      for two of the four ranged creatures that one is silent. This is the concrete form of "transcribe the
      remaining six modules" and it now has a per-creature list rather than a shrug.

- [x] 56. **ANSWERED AND FIXED: the decoder was not modelling the branch DELAY SLOT.**
      MIPS executes the delay slot whichever way a branch goes, and the compiler puts a `lui` in it: when the
      branch is taken the `lui` runs and its matching `addiu` is at the TARGET. A linear register walk pairs
      that `addiu` with whatever the FALL-THROUGH path last left in the register, which yields a plausible
      in-image address that fails validation and is dropped without a word.

      The Arachner's run callback (`0x80101010`) is exactly that shape:

          lw   v0, 220(a0)        ; aiflags
          andi v0, v0, 1          ; STAND_GROUND
          beq  v0, zero, +0x18
          lui  v0, 0x8010         ; <- delay slot
          lui  v0, 0x8010
          addiu v0, v0, 5944      ; 0x80101738, the stand-ground move
          jr   ra
          sw   v0, 216(a0)
          addiu v0, v0, 6152      ; TARGET: 0x80100000 + 6152 = 0x80101808

      The linear walk computed `0x80101738 + 6152`, not `0x80100000 + 6152`, so the real run move at
      `0x80101808` was never recorded. The tracker now keeps each register's last `lui` beside its tracked
      value and offers BOTH candidates for a materialising `addiu`; every candidate is validated structurally
      before being accepted, so the wrong one costs nothing.

      Disc-wide: **97 moves become 101, and moves attributed to the run callback go from 5 to 8.**

      What that does in play, POWER1, the same 500-frame capture:

      | | before | after |
      | --- | --- | --- |
      | checkattack reached | 0 | 101 |
      | attack callback ran | 0 | 3 |
      | run move found / missing | 0 / 2 | 5 / 0 |
      | decoded thinks run | 28 | 33 |

      The Arachner went from standing still for the whole capture — no run animation, so `M_MoveFrame` had
      nothing to advance and it never moved a unit — to running, chasing and attacking. The Soldier is
      unaffected: BASE0 reports the same 19 shots.

  *As first written:*

- [x] ~~56. **Some callbacks have no move attributed to them at all.**~~ The Arachner never moves on POWER1:
      `run 0 / missing 2` — `set_via(m, 4)` is called and finds nothing, so there is no animation to play and
      `q2_M_MoveFrame` advances nothing. Its module has a run callback (or `m->run` would now be NULL) but no
      move records `via == 4`, which means it installs one indirectly — through another move's endfunc, which
      the decoder marks `via == -1`. Sixteen of the disc's 97 moves are `via == -1`. The Gunner shows the same
      shape on WASTE3, where it never reaches a single `checkattack` in 700 frames.

  *As first written:*

- [x] ~~54. **A decoded creature has still not been seen firing.**~~ Two real obstacles are gone — the think
      table was being wiped, and the phantom melee was absorbing every attack — and the fire routing is
      covered by unit tests, but no capture has yet produced a `fire_sent`. What the counters now say is where
      to look: on COMMAND the Tank Commander's attack move installs and the thinks that run carry import
      `+0x12C`, not the fire slots, so the question has narrowed to which of its animation frames carry think
      8, 10 and 13 and whether the attack move reaches them. On WASTE3 the Gunner never gets as far as one
      checkattack in 700 frames, and its module has no run move via slot 4 either — a separate thread.

## Where a fire think sits in its move, and why the animation never gets there

The census now prints each move's think bytes **in frame order, run-length encoded**, because which thinks a
move calls is only half the question. `0*12 3 4` and `3 4 0*12` are the same set and a very different
creature: an animation that is cut short reaches the second and never the first.

The Arachner's attack move, 94-109, is `0*5 3 0*5 4 0*4`. Its fire thinks are 3 and 4 — both call `+0x8C`,
the rail — and they sit at frame **6** and frame **12** of sixteen. The move has to survive six AI ticks
before anything is fired.

A per-index tally of which thinks actually run says it does not. On POWER1, 500 frames, five live creatures:

    moves      attack set 3 / missing 0, run 5 / 0
    think hit  1:12  6:9  7:12
    decoded    33 thinks, 36 calls (36 unclassified), 0 fire calls

Thinks 3 and 4 never run. Nor does 0 — which is the first thing the tally settled: **think byte 0 means "no
think"**, and every move on the disc is mostly zeros, so an absent 0 in the tally is correct rather than
alarming. The 1, 6 and 7 belong to the other creatures in that zone.

So the attack move is installed three times and cut short before its sixth frame each time.

- [x] 57. **RETRACTED: nothing cuts it short. The premise was wrong.**

      A per-tick trace — one creature, one line, showing its move, frame and attack state — settles it in
      thirty lines. On COMMAND, creature 0:

          t9    move 30   frame 31   as 4      <- the decision
          t10   move 77   frame 32   as 1      <- the attack move installs
          t11   move 77   frame 77   as 1      <- snaps to its first frame
          t12   move 77   frame 78   as 1
          ...   ...
          t29   move 77   frame 95   as 1      <- still playing, nineteen ticks in

      The Tank Commander's attack move plays through cleanly. It is never interrupted, by pain, by run, by
      stand or by anything else. The question this entry was opened to ask does not exist.

      What was actually happening is what question 54 already established and I then talked myself past: the
      move that plays, 77-114, carries thinks `0*6 6 0*3 6 0*7 11 0*6 9 0*11 6` — a proximity call and two
      sounds, and **no fire think at all**. Its fire thinks live in the other three moves the same callback
      installs, and it never plays those:

      | move | thinks in frame order | verbs |
      | --- | --- | --- |
      | 77-114 *(the one that plays)* | `0*6 6 0*3 6 0*7 11 0*6 9 0*11 6` | `5*38` |
      | 168-196 | `0*5 13*19 0*5` | `4*5 0*19 4*5` |
      | 115-135 | `0*15 6 0*5` | `4*21` |
      | 55-70 | `0*9 8 0*2 8 0*2 8` | `4*16` |

      The 168-196 move is worth looking at: nineteen consecutive frames of think 13 with no verb running under
      them. That is a sustained burst — the creature holds still and fires for nineteen ticks.

      The "cut short before its sixth frame" claim came from the Arachner on POWER1, where the counters said
      the attack move installed three times and thinks 3 and 4 never ran. The trace shows why that was a bad
      inference: creature 0 there spends the whole capture in its run loop with an enemy and `as 0`, so those
      three attacks belonged to other creatures whose moves I never checked. Counters over a capture cannot
      attribute an event to an actor; a trace can.

      **Question 55 is therefore the whole of the remaining gap** — which of the four moves an attack callback
      installs is the one the module picks. Nothing else is in the way.

  *As first written:*

- [x] ~~57. **What cuts the attack animation short — and it is NOT the AI verb, which was my guess.**~~

      The census now prints the `ai` byte per frame beside the think byte, run-length encoded, and the answer
      is that the port already has this right. Across the disc the verbs land exactly where they should:

      | move installed by | verb its frames carry |
      | --- | --- |
      | stand | 1 |
      | walk | 2 |
      | run | 3 |
      | attack, melee | 4 |
      | pain, die | 5 |

      and `q2_ai_verbs[]` is already one-based to match — `NULL, ai_stand, ai_walk, ai_run, ai_charge,
      ai_move` in the executable's own slot order at `0x800D561C`. So the Arachner's attack move runs
      `ai_charge` sixteen frames running (`ai: 4*16`), which is correct and does not hand the creature back to
      the chase. **The suspicion recorded when this question was opened is wrong and is withdrawn.**

      One move on the disc is worth noting on the way past: an attack whose verbs read `4*5 0*19 4*5`. Verb 0
      is the slot the original stores zero in, so nineteen of its frames deliberately run no verb at all —
      a hold in the middle of an attack.

      What remains: the attack move installs three times in a 500-frame capture and never advances past its
      sixth frame. The counters now cover every callback that can install a move, and the shape is:

          attack set 3, melee 0, run 5, pain 0, die 0, stand 14

      **Pain and die are ruled out** — the obvious guess, that the player shooting the creature in a `--watch`
      capture keeps interrupting it, is wrong: neither fires once. **Stand is installed fourteen times**, more
      than run and attack together, so the creature is oscillating back to standing rather than being
      interrupted by damage.

      Two more things checked and correct on the way past, so neither is looked at again: the medic branch of
      the dead-enemy test (`enemy->health > 0` really does mean "stop caring" for a medic, and it is guarded
      by `Q2_AI_MEDIC` exactly as id guards it), and the sight client the AI treats as the player, which is
      set up with `in_use`, both halves of the INUSE flag, `client`, and health 100 — so creatures are not
      dropping their enemy because the proxy looks dead.

      Also measured: of 101 `checkattack` calls in that capture, 97 reach the decision and only 3 say yes.
      That is consistent with `M_CheckAttack`'s own odds rather than a fault — the chances are 0.4, 0.2, 0.1
      and 0.02 by range band.

      What is left is a per-tick trace of one creature: which move is installed, which frame it is on, and
      what replaced it. The counters say what happens across a capture; they cannot say what happens between
      two consecutive ticks, and that is now the only question.

  *As first written:*

- [x] ~~57. **What cuts the attack animation short.**~~ The generic run handler installs the run move whenever
      `m->run` is called, and the counters show run installed 5 times against attack's 3 in the same capture.
      In the original a monster's attack move plays out because its frames carry an AI verb — the `ai` byte of
      `{u8 ai; s8 dist; u8 think}` — that keeps the AI in the attack rather than returning it to the chase.
      The suspicion is that the port's frame driver maps that verb in a way that lets the chase reinstall the
      run move on the next tick. That is the last thing between a decoded creature and a shot: the fire is
      routed, the import is named, the think is decoded, the move is installed, and it is being interrupted
      six frames too early.

## A decoded creature fires

`22 fire calls: 22 sent`, `think hit 8:22`, on COMMAND over 400 frames. The first shot from a creature this
project never transcribed — and it took one function.

The chain that had to hold, end to end: the AI grants an attack (`M_CheckAttack`, transcribed), the attack
callback picks a move, the move's frames name a think, the think decodes to a step, the step names an import,
the import is one of the five projectile spawners, and the hook fires. Every link was already in place. The
attack callback was picking the wrong move.

`cre_tankcomm.c` writes ONE callback. Everything else — stand, idle, search, walk, run, pain, die and all
thirty-two think indices — stays on the generic handlers and the decoded actions. That is now possible
because `q2_creature_bind` falls back to the generic implementation's method for any think index a partial
transcription leaves NULL, which turns "transcribe a creature" from an all-or-nothing job into a per-function
one.

The transcription is the table read last round, with two departures stated in the file: the distance is
compared as a square against 1501² and 3001² so no root is taken, which orders identically; and the timer at
`entity+0xA8` that the long-range branch sets is not written, because which global it reads is not
established. The move it accompanies is installed either way.

Before and after on the same map and capture length:

| | before | after |
| --- | --- | --- |
| think hits | 1:12 6:9 7:12 | **8:22** |
| CALL steps reached | 31 | 88 |
| fire calls | 0 | **22** |
| fire sent | 0 | **22** |

Five modules remain, and each is now a reading job at a printed address rather than a search: `q2psx-inspect
creatures` gives every callback's address, every move's record address and installing callback, and the think
bytes each move's frames call in frame order.

## The Gunner, and one more import named

Second transcription, same shape as the first: one callback, everything else on the generic handlers and the
decoded actions. `gunner_attack` is at `module+0x1814` and is shorter than the Tank Commander's —

    v0 = import[+0xB4](self, self->enemy)
    if (v0 == 0)                  move = M_137_143;
    else if (import[+0x14]() & 1) move = M_108_128;
    else                          move = M_137_143;

**`import[+0xB4]` is `q2_range`**, which this port already carries — the eleventh import slot named, and this
one identified by more than shape. `0x8005EF84` subtracts the two origins, takes `q2_vector_length_sq`
(`0x8005C59C`, already named in `ai.h`), and compares against `0x003F803F` or `0x000FE00F` depending on
whether the entity's class byte at `+0x23` is 68. Those are 2040² - 1 and 1020² - 1: the long-melee and
ordinary melee bands, with the same off-by-one `q2_range` already reproduces in `monster.c`. So the branch is
`range == Q2_RANGE_MELEE`, read rather than guessed.

Unlike the Tank Commander, the Gunner was not silenced by the generic handler: **both** its attack moves are
firing animations, so taking the first happened to agree with the original. The transcription makes that
agreement deliberate and adds the melee-range branch the generic handler had no way to know about.

On WASTE3 the Gunner now attacks — `2 checkattack, 1 yes, 1 attack call` — and **think 13 runs 33 times**,
which is `M_137_143`'s first frame. It still fires nothing, and the census says why:

- [x] 58. **ANSWERED AND FIXED: a move's endfunc could only be run if it was also a callback or a think.**

      `resolve_endfunc` maps an endfunc address to one of the thirteen callbacks or thirty-two methods the
      implementation carries. A standalone installer is neither, so it resolved to NULL and the chain it
      exists to make simply did not happen.

      The Gunner's is three instructions at `module+0x11D8`:

          lui   v0, 0x8010
          addiu v0, v0, 7292        ; 0x80101C7C — move 144-151
          jr    ra
          sw    v0, 216(a0)         ; delay slot: self->currentmove = it

      That move is eight frames of think 2, and think 2 is the one carrying `call(+0x84)`, the hitscan. The
      Gunner's entire ranged attack was on the far side of a function the port could not name.

      The decoder now records what such an endfunc installs — one store to `entity+0xD8`, a materialised
      address that validates as a move record, and nothing else, so a function that does more is left alone —
      and the bind installs a shared endfunc that performs it. **43 of the disc's 101 moves have a decoded
      endfunc target**, where none did.

      A second delay slot caught this one out on the way: the store is in `jr ra`'s slot, and the first
      version tracked that word without checking it, so every endfunc on the disc reported as installing
      nothing. That is twice in two days the delay slot has hidden something — worth remembering as the first
      thing to check when a linear walk comes back empty.

      WASTE3, 700 frames: `think hit 1:22 2:1 6:15 7:15 13:1`, `1 fire calls: 1 sent`. The Gunner reaches
      think 2 and fires. The Tank Commander is unaffected — COMMAND still reports 22 of 22.

  *As first written:*

- [x] ~~58. **The Gunner's fire think is 2, and think 2 lives in a move nothing reaches.**~~ Its move
      `144-151` is eight frames of think 2, and think 2 is the one carrying `call(+0x84)` — the hitscan. That
      move's `via` is **-1**: no callback installs it, so it is reached only through another move's endfunc.
      Presumably `M_137_143` or `M_108_128` ends into it. The endfunc chain is resolved at bind time
      (`resolve_endfunc`), so either the chain is not firing or the move it names is not the one the census
      thinks. Sixteen of the disc's 101 moves are `via == -1` and this is the first one shown to matter.

## The Infantry and the Arachner, and one attack that declines

Two more attack callbacks read and written. Four of the seven modules now have one; the Berserk and the
Insane have no attack callback at all, which is correct — they are melee creatures — so the ranged set is
complete except for the Soldier, which was transcribed long ago.

**Infantry**, `module+0x1848`, the shortest of the four:

    v0   = q2_range(self, self->enemy);
    move = (v0 != 0) ? M_184_198 : M_199_206;

One range test, no roll. Both moves shoot — 199-206 carries think 11, a melee plus `call(+0xFC)`; 184-198
carries think 8, `call(+0x84)` — so the generic handler's choice was not silent, merely always the
close-quarters one.

**Arachner**, `module+0x12CC`, the only one so far that can DECLINE:

    v    = self->origin - enemy->origin;
    dist = length(v);
    if (dist < 1053) return;                       ; installs nothing
    self->[0x5C..] = enemy->origin;
    self->[0x60]  += enemy->[0x4C];
    move = (|v.x| + |v.z| < |v.y|) ? M_94_109 : M_130_132;

Two things the generic handler could not have expressed. It **installs nothing** below 1053 units, so a close
Arachner carries on with whatever it was playing — `set_via` always installs something. And the choice is
**vertical**: `|dx| + |dz| < |dy|` asks whether the target is further away up-or-down than along the floor,
which for a creature that walks walls is the question that decides the attack. Not a range band, not a roll.
94-109 is the rail (thinks 3 and 4, both `call(+0x8C)`); 130-132 is a three-frame gesture with no think at
all.

The `self->[0x5C]` block the original fills with the enemy's origin, and the accumulation into `self->[0x60]`,
are not written: what reads them is not established and neither affects which move is installed.

**Neither was observed firing in a capture.** POWER1 reports `1 fire calls: 1 sent`, but the think hits there
are 2, 6, 7 and 13 — the Gunner's, since that zone carries one. POWER2's Infantry does not reach a single
attack in 500 frames. That is the same limit as ever: these creatures engage rarely in a scripted demo run,
and a capture long enough to catch one costs more than the harness has. The transcriptions are read from the
disassembly and tested for regressions; they are not yet confirmed in play, and this says so.

## Two address spaces, and the check that tells them apart

`moddisasm` was reading the wrong creature, and would have gone on doing it silently.

`q2_ai_module_load` relocates a COMMON.DAT's whole `CreAIBin` as ONE blob. That is right for a LevelBin and
wrong for CreAIBin: the chunk is a LIST of modules — a 12-byte name, a next-offset, a body — and `creatures`
relocates each one separately to its own base. **The two address spaces agree only on a map carrying exactly
one module.**

The trap is that a wrong address disassembles perfectly well. Feeding the Berserk's melee address from
`creatures` into `moddisasm WASTE4` produced clean, plausible MIPS: a health test, a move install, an aiflag
clear — the Tank Commander's attack, in a different module entirely.

**The check that catches it, and that vouches for the four transcriptions already written: do the moves the
code installs appear in that creature's own move list?** Every one of the Tank Commander's, Gunner's,
Infantry's and Arachner's did, which is why those reads were sound. The Berserk's did not, which is what
exposed this.

`moddisasm` now takes a creature name and relocates that module alone, so the addresses match what
`creatures` prints and the question stops arising.

## The Berserk, and the ranged set closed

With the right module in view the Berserk's melee at `module+0x11D8` is the simplest of the five:

    r    = import[+0x14]();            ; the random
    move = (r & 1) ? M_84_95 : M_76_83;

A coin. No range test, no health test. Both moves are swings — `76-83` carries thinks 3 and 4, `84-95` thinks
3 and 6 — so the generic handler taking the first was half right, and this adds the other half: the creature
alternates its two swings instead of always throwing the same one.

That is **five of the seven modules** with a hand-written callback. The Soldier was transcribed long ago. The
Insane has no attack, no melee, and its stand, walk and run callbacks are all the same address — a
non-combatant, correctly modelled by the generic handlers alone.

WASTE4 reports `24 fire calls: 24 sent` and COMMAND holds at `22 of 22`. All 26 tests pass.

## Rotator coverage is complete, and the number that looked like a gap is not one

"26 rotators" has been quoted back at this project as though it were a fraction of something larger. It is
not, and `zonescript` now says so in the output rather than leaving it to be inferred:

    rotation CALLs  : 95  in COMMON's scripts, disc-wide
      too short     : 0   (the item cannot hold the operands)
      no object     : 69  (first object slot is -1)
      usable        : 26
    rotators built  : 26  (one per object slot each call names)

Ninety-five CALL items across the disc name a rotation primitive. **Sixty-nine of them have -1 in EVERY
object slot**, so they install nothing on the console either. Not one item is too short to hold its operands.

That sentence used to say "in their first object slot", on the strength of a note in `rotator.h` claiming the
constructor stops at the first negative one. **It does not stop, it skips.** `0x80028628` is
`bltz v0, 0x8002875C`, and `0x8002875C` is the loop's own increment — `s1 += 2; s4 += 1; if (s4 < 4) loop` —
so the constructor moves to the next of the four and carries on. The port implemented the note rather than
the code, with a `break` where the original has a `continue`, and would have discarded every rotator in a
call that had a gap before it.

Fixed in both places that walk the slots. The count does not move: checking all four instead of the first
still finds 69 calls with nothing in any of them, which is why the bug was invisible on this disc. It would
not have stayed invisible on a mod.

So the ratio that matters is **26 of 26 usable calls build a rotator**, and the port skips nothing the
hardware does not. A disc-wide count of what a system *could* act on is only a denominator if the data
underneath it is live; here two thirds of it is not.

## Players can hit each other

`combat.targets` held creatures and nothing else, so in a deathmatch every shot passed straight through
everybody. It now holds every creature plus every OTHER player, rebuilt per player rather than per frame —
a player's own actor is skipped, because registering it would let a bolt hit its own muzzle.

The pointers are stable: the live player's hurt-actor is `sim->combat.self` and a parked one's is
`sim->pcombat[i].self`, both inside the sim.

Measured, which is the point of building it this way:

| players | targets | of which |
| --- | --- | --- |
| 1 | 0 | — |
| 2 | 1 | 1 other player |
| 4 | 3 | 3 other players |
| 2 on COMMAND | 26 | 26 creatures, 0 players (that map has no MultiSpawn, so only one spawns) |

**A parked player takes damage on their actor, and their inventory is a separate field.** Only the live
player's pair is synchronised, so a hit landed while parked has to be copied back or it is lost when their
frame runs. That sync exposed a real bug immediately: `q2_sim_player_reset_combat` initialised an extra
player's inventory but left their ACTOR zeroed, so the first sync wrote health 0 over a full 100 and three of
four players ended a capture dead without anything having shot them. The actor is now built from the
inventory at reset, so the pair starts in step.

Single player is byte-identical, all 26 tests pass.

## A kill has a killer

`q2_mp_attribute_kill` has taken a killer's id since it was written, and nothing could supply one. The engine
keeps it as a signed byte at `entity+222`; the port's `q2_actor` had no equivalent, so a deathmatch kill had
a victim and nobody to credit it to.

An actor now carries `owner` — which player it is, or -1 for anything that is not one — and the damage
function writes `target->last_attacker = attacker ? attacker->owner : -1`. Each player's actor is stamped
with its own index at spawn, and any player whose health crosses zero goes through
`q2_mp_attribute_kill` and then `q2_mp_player_killed`, which is the engine's own hook at `0x800396AC`
reproduced: `(*module)->[4](killer, victim)`.

Checked in `test_combat` rather than asserted:

- a shooter with `owner = 2` leaves `last_attacker = 2` on its victim, and attribution gives player 2 the frag;
- world damage leaves -1 and stays nobody's frag;
- **lava is nobody's frag even when a player last touched you** — the environmental means of death are
  excluded by the rule `q2_mp_attribute_kill` already carried, and this is the first caller able to reach it.

Not observed in a capture: the demo players are scattered across an arena firing blindly and never hit each
other in 1200 frames. The scoring path is wired and unit-tested; it has not scored a frag in play, and this
says so rather than implying otherwise.

Single player is byte-identical, COMMAND still reports 22 fire calls of 22 sent, all 26 tests pass.

## Staging an encounter, and what it has not yet shown

Every unverified claim in the last few rounds has the same shape: "wired and unit-tested, not observed in
play". The reason is always the harness — a scripted demo wanders, it does not arrange a fight. `--watch`
already fixed that for one player against a creature by standing the player in front of one. `--dm-stage`
does the equivalent for players: it puts the others a few hundred units from player 0, aims everyone at
everyone by POSITION, and holds their fire button.

Two real faults fell out of building it, both of which would have silently defeated any attempt to test
player-versus-player damage:

- **Aiming by reversing a yaw is not aiming at someone.** The first version set the other player's yaw to
  player 0's plus 2048, which points them back down player 0's line of sight and only coincides with pointing
  AT them when player 0 happens to be looking at the right spot. 900 frames of that produced nothing.
  `q2_vectoyaw` on the difference of the two positions is the fix.
- **The extra players had no weapon.** `q2_sim_player_reset_combat` called `q2_inventory_init` and set
  `weapon_id = 0`, which is "no weapon" — so three of four players spawned holding nothing and could not have
  fired a shot between them. They now start with what player 0 has, which is what a deathmatch start does.

- [~] 59. **A staged encounter produces shots but no damage, and the counters have narrowed it to one
      comparison.** Three real faults fixed on the way, then an instrument that should have come first.

      Fixed, each of which alone would have prevented a hit:

      - **the target pointer aliased.** `client_targets_for` chose `&combat.self` for whichever player was
        live when the list was BUILT — but `q2_sim_advance_player` swaps afterwards, so that pointer named a
        different player by the time the shot traced. Player 1 was firing 301 shots at its own actor. Every
        other player is parked during `who`'s tick, so `&pcombat[i].self` is always right.
      - **the hurt-actor's position was stale.** `q2_actor_from_player` only runs inside
        `q2_sim_hurt_player`, so an actor's origin was wherever that player last got SHOT. With one player
        nothing ever traced at them and it never showed. It is now updated every tick.
      - **the extra players had no weapon**, and aiming was by reversed yaw rather than at a position — both
        recorded above.

      Then the counters, which say where a shot stops considering a target: `190 tested, 0 skipped, 0 dead,
      146 behind, 44 beyond world, 0 off axis, 0 hit`.

      **Nothing is off-axis**, so aim is not the problem — the ray is either pointing away from the target
      (146) or the world stopped it first (44). Adding 2048 to the computed yaw made it 1409 of 1409 behind,
      which confirms the convention already in use is the right one and that the 44 are the genuine
      in-front cases.

      **The units are consistent** and that line of enquiry is closed: `along = (dot * 4096) / len2` is a
      1.12 fraction of the ray, `world_fraction` is clamped to the same 0..4096, and `q2_combat_ray_dist_sq`
      says why — "nothing is normalised because the direction's LENGTH is the weapon's range". So a shot
      counted `beyond world` really was stopped by geometry.

      Placing the staged player IN FRONT of player 0 rather than at a blind diagonal — the same reasoning
      `--watch` uses to frame a creature — moved 9 shots from `beyond world` into `off axis`, which is the
      first evidence of a shot reaching a target at all.

      **And then three further changes produced byte-identical counters: 2126 / 1988 / 129 / 9 every time.**
      That is the finding. Numbers that do not move when the thing they supposedly measure is changed are not
      measuring it: the scans are player 0's shots, not player 1's. Player 0 fires from the demo pad
      constantly, is aimed at the top of each frame by the staging, and then turns away during its own tick —
      which is exactly the 1988 `behind`. The staging holds the EXTRA players' aim and never held player 0's.

- [x] 59a. **DONE, and it reframed the question.** The scan counters gained a shooter dimension —
      `q2_combat_scan_by[]` indexed 0..3 by player and 4 for everything else, set by whoever is about to
      fire — and player 0's aim is held as well as the extra players'. The first capture with both said
      something no total could have:

          scan[0]: 4304 tested, 2806 behind, 1491 beyond world, 7 off axis, 0 hit
          scan[1]: 0 tested,    0 behind,    0 beyond world,    0 off axis, 0 hit

      **Player 1 fires 301 shots and produces zero traces.** That is not a miss — the hitscan path is not
      the path its weapon takes. `q2_sim_fire`'s `default:` arm is
      `q2_projectile_launch(&sim->combat.projectiles, &r, -1, sim->level_time)`, so a blaster bolt is a
      PROJECTILE and never goes near `nearest_hit`. Four rounds of reasoning about aim, geometry and units
      were about the wrong path, and one counter with a shooter dimension said so immediately.

      Two real faults fell out of that:

      - **A launched projectile has no owner.** The `-1` is the owner index, so a bolt that hits cannot say
        who fired it, which is the projectile-level twin of the `entity+222` gap `owner`/`last_attacker`
        closed for hitscan.
      - **The projectile step ran once per PLAYER.** `q2_sim_combat_tick` steps every bolt in flight, and it
        is called from the player half of the tick — so with four players a bolt advanced four times a frame
        and a rocket crossed an arena at four times its speed. It is the same class of bug the world-half
        gate exists for, missed because it lives in another file. Now gated on `cur_player == 0`.

- [~] 59b. **Owner given, world list built, three faults fixed — and a bolt still hits nothing.**

      `q2_sim_proj_scan` counts what happens to the projectiles: `601 launched, 4356 stepped, 52 expired,
      0 hit`. They fly. They are stepped. They never intersect anything.

      Three real faults found and fixed on the way, none of which was the aim:

      - **A launched bolt had no owner.** `q2_projectile_launch(..., -1, ...)` — so a bolt that hits could
        not say who fired it. It now carries `cur_player`, and `attacker_for()` resolves that back to the
        right actor at impact. Without it every bolt in the air was credited to whoever happened to be
        ticking, which for the projectile step is always player 0.
      - **The projectile step ran once per PLAYER**, so with four players a bolt advanced four times a frame.
        Gated on `cur_player == 0`, like the entity sweep and the effects.
      - **The step used the SHOOTER's target list.** `combat.targets` deliberately excludes the player it
        belongs to, and the step runs on player 0's tick — so a bolt fired by player 1 at player 0 was tested
        against a list whose one omission was player 0. `q2_sim_set_world_targets` now carries every player
        and every creature with nobody left out, and the owner is skipped by pointer at impact instead.

      The geometry is measured now, and it is very nearly right: `near 0 (past end 0), closest^2 333434,
      seg^2 3973`. The closest a bolt ever came is **577 units** against a reach of `Q2_HITSCAN_RADIUS + 286
      = 572`. It misses by five. A bolt travels 63 units a tick, and nothing is ever rejected for being past
      the segment's end, so the step length is not the problem either.

      577 against 572 is the height of a man: `q2_player.pos` is the FEET — what a StartPos names — while a
      shot leaves the other player's EYE, `Q2_EYE_BASE` 286 above it. The hurt-actor's origin now sits at the
      eye for that reason, which is right on its own merits: what a shot has to intersect is the body, not
      the floor under it.

- [x] 59c. **ANSWERED by doing what it said. A player can hurt another player.**

      The rule was: when a number does not move, stop fixing and find out whose number it is. Printing the
      origin the scan actually read took one line and settled it:

          closest: owner 0, bolt at [-9447 -1370 15759], target origin [-9382 -798 15307]

      **572 apart vertically — two eye-heights.** The muzzle sits one `Q2_EYE_BASE` above the feet and the
      target's origin was one below them, which is why the closest approach was pinned at 577 against a reach
      of 572 no matter what was changed upstream.

      The cause was the HARNESS. `--dm-stage` wrote `pcombat[pi].self.origin = pl->pos` every frame — the
      feet — over the origin the sim now maintains at the eye. A harness that overwrites the field it is
      measuring measures the harness, and that is what pinned the number through four separate fixes.

      With those three lines gone, on the same capture:

      | | before | after |
      | --- | --- | --- |
      | bolts hit | 0 | **4** |
      | targets within reach | 0 | 690 |
      | closest² | 333434 (577) | 110422 (332) |

      And over 2400 frames **player 1 ends at 68 health**, down from 100 — four hits at eight damage, which
      is exactly the arithmetic. A player can shoot another player, the damage lands, and the health is
      theirs alone.

      Not yet a kill: a blaster needs about thirteen hits and the staged pair land four in 2400 frames. The
      scoring hook, the attribution and the frag limit are wired and unit-tested above; what has not been
      seen is the moment they fire.

  *As first written:*

- [x] ~~59c. **And the measurement did not move.**~~ `closest^2` is 333434 before and after, byte-identical —
      the THIRD time this session an unchanged number has meant "you are not measuring what you changed".
      The first cost two rounds on a creature count that belonged to a different creature; the second cost
      four on a scan counter that belonged to a different player. The pattern is now unmistakable and the
      response should be automatic: when a number does not move, stop fixing and find out whose number it is.
      Here that means printing the origin the scan actually read, per target, rather than reasoning about
      which actor `hit_list` holds during the step. The launch passes -1; the
      shooter's index is available at the call site. Until then a player-versus-player kill can happen and
      still not be attributed. The scan counters cover hitscan only, so this wants the same treatment: count
      where a projectile stops, per owner, rather than reasoning about it. One shared
      counter cannot say whose shot it was, which is the same mistake as reading a whole-capture creature
      count and attributing it to one creature. The counters want a per-player dimension before the next
      attempt, not another fix.

      One real bug fell out of looking: `pcfg.style = c->sim[pi].player[0].look_scheme` still read the OLD
      per-player sim array, which has been an uninitialised struct since the players moved into `sim[0]`.

  *As first written:*

- [x] ~~59. **A staged encounter still produces no damage.**~~ Two players 339 units apart, facing each other by
      position, both holding fire, both with the level's weapon, each registered in the other's target list
      (`has 1 targets (0 creatures, 1 other players)`) — and both end 900 frames at 100 health. Everything
      upstream is measured: the target list is right, the actors have `radius` 286, the attribution rule is
      unit-tested and the kill hook is wired. What has NOT been measured is whether the shot is taken at all
      for a player whose frame runs through `q2_sim_advance_player`, and that is the next thing to count
      rather than reason about — the fire path reads `combat.last_shot`, which is part of the swapped half.

## A deathmatch, played through

    multiplayer: player 1 killed by 0 — frags 1 0 0 0
    multiplayer: frag limit reached at 500 dt (1 s) — banner 'GAME OVER'
    multiplayer: request 11 (load MPResults); winner 0 — PLAYER 1 WINS
    multiplayer: DM SCORES — DEATHMATCH, HUD set qk2_menu.lbm

Every link, in one capture: a player fires, the bolt carries its owner, it finds a target in the world's own
list, the damage lands on that player's inventory, the death is noticed, the attribution names the killer,
the frag is scored, the frag limit ends the match, the banner runs, the runtime asks for state 11, and the
scoreboard shows `PLAYER 1  1 / PLAYER 2  0` over the arena.

One last fault, and it is the same shape as the rest: **`q2_actor_from_player` erased which player an actor
was.** It calls `q2_actor_init`, which clears `owner` to -1, and it runs on every hit — so the first bolt to
land wiped the victim's identity, the kill was attributed to the world, and because the runtime blames the
victim for a world kill the *victim* was docked a frag. The capture said it plainly: `player 1 killed by -1 —
frags 0 -1 0 0`. Preserving `owner` across the refresh turns that into `killed by 0 — frags 1 0 0 0`.

The staged pair also moved from 600 units apart to about 400, which is inside the actors' combined reach and
makes the exchange conclusive in a capture short enough to run: 13 hits rather than 4, and a death.

Single player is byte-identical to before any of this began, all 26 tests pass, and COMMAND still reports 22
fire calls of 22 sent.

## Six creatures were silent, and one of them has no voice on the disc

Only the Soldier's sound names had been read — out of its own module, an eleven-entry table at
`module+0x1D0` — so the other six played nothing. `cre_soldier.c` said as much: *"another creature's table
has not been read, so it stays silent rather than borrowing these."*

Every module carries the same table, and the slot INDEX is the sound number. That is not assumed: the
Soldier's numbering was read out of its code — idle 0, sight 1, pain 2, death 5, shotgun cock 9 — and its
slots sit in exactly that order. `q2_creature_sound_names` reads any module's the same way.

The table is FOUND rather than offset: it sits `0x5C` past the module's own name string on three of the
seven and `0x60` on the Arachner, so the reader takes the first place where three consecutive 12-byte slots
all hold a printable NUL-terminated name.

Measured over 300-frame captures, requested sounds against what the map's bank actually holds:

| map | creature | sounds | not in bank |
| --- | --- | --- | --- |
| WASTE3 | Gunner | 16 | 0 |
| POWER1 | Arachner | 7 | 0 |
| WASTE4 | Berserk | 7 | 0 |
| COMMAND | Tank Commander | 4 | **4** |

~~**The Tank Commander has no voice on this disc.**~~ **WRONG, and refuted by scanning the disc image
directly.** Searching the raw image for VAG headers and reading each one's name field finds **63 entries
whose name begins `tnk_`**:

    tnk_atck1  x13    tnk_death  x13    tnk_pain  x13    tnk_sight1 x12    tnk_step x12

Five of the six sounds the module asks for are on the disc, in twelve or thirteen banks each. Only
`tnk_idle1` genuinely has no VAG. **The Tank Commander's audio is there to load**, and the reason it does
not play is somewhere in this port, not on the disc.

How the original claim went wrong is worth recording, because the same mistake was nearly repeated while
checking it: the first re-test this session was `q2psx-inspect audio | grep -i tnk_`, which returned zero —
**but that command prints only aggregate statistics and no names at all**, so the grep could not have found
anything either way. A search over output that cannot contain the answer returns zero and looks like
evidence. The count is what broke it open: `tnk_death` occurs 35 times in the image while the disc carries
only 15 creature modules in total, so most occurrences could not be module copies. The bytes before the
first one read `VAGp`.

The stale claim below is left for the record. That is worth knowing before anyone
goes looking for a bug in the lookup.

## The census was understating the creatures by a mile

`q2psx-inspect creatures` reported `Tankcomm — transcribed by hand, 0 of 7 indices` for a creature whose
attack is written out by hand and whose every think index acts. It counted `impl->method[k]`, which a PARTIAL
transcription deliberately leaves NULL so `q2_creature_bind` falls back to the decoded action. The measure
was counting the thing that is empty *because* the design works.

Read the way the runtime actually resolves them:

| creature | callbacks written by hand | think indices that act |
| --- | --- | --- |
| Soldier | 7 of 8 | 14 of 14 |
| Tank Commander | 1 of 8 | 7 of 7 |
| Gunner | 1 of 9 | 8 of 8 |
| Infantry | 1 of 9 | 8 of 8 |
| Arachner | 1 of 10 | 4 of 4 |
| Berserk | 1 of 8 | 4 of 4 |
| Insane | generic throughout | every index acts |

**7 of 7 creatures act.** Every module on the disc that can attack has its attack callback read out of its own
code and written by hand; the Insane has neither an attack nor a melee, and its stand, walk and run are one
address — a non-combatant, correctly served by the generic handlers.

The one-of-N callback figures are the point of the partial-transcription design, not a shortfall: what is
written by hand is the branch the decoder cannot recover, and everything else runs from what it can.

A `d` in the think list now marks an index that acts from a decoded action rather than a hand-written one, so
the distinction is visible instead of being flattened into "missing".

## The client acts on one entity event in three

`client_entity_events` handled `Q2_ENT_EVENT_SOUND` and fell through everything else without a word. The
entity world raises three kinds, and `item.c` raises the other two: the pickup particle burst at `item.c:759`
(the original's `0x8005B6C0`) and the item glow light at `item.c:858`.

Counted rather than asserted, over 900-frame captures:

| map | lights dropped | bursts dropped |
| --- | --- | --- |
| BASE0 | 0 | 0 |
| BASE1 | 0 | 0 |
| LAB | 0 | 0 |
| WASTE3 | 0 | **2** |

**Two items were collected on WASTE3 and both bursts were discarded.** That is the useful half of the
measurement in both directions: the inventory path works — a demo run walks over an item, the pickup fires,
the entity world raises the burst — and the client throws the visual away. The events are now counted rather
than silently ignored, so "the client handles entity events" stops being true of one kind in three with
nothing saying so.

- [x] 60. **DONE: the pickup burst is read and drawn.**

      `0x8005B6C0` is a four-line wrapper:

          q2_burst(pos, 0x8009BF88, 0x8009BA60, 6144, 0)   ; -> 0x8005AB70

      `0x8009BA60` is already named in `fxtables.h` as the ramp table, nineteen 132-byte records. So the two
      pointers are ramp indices, not addresses to chase: `0x8009BF88 - 0x8009BA60 = 1320 = 132 x 10`, so the
      burst uses **ramp 10 and ramp 0**, at **size 6144**, area 0.

      Three of the port's seven preset fields therefore fall out immediately. The fourth does not:
      `0x8005AB70` opens by reading `a0->[0x10]` — a pointer off the entity — passing it to `0x8006D6AC`,
      which walks a structure counting something (`lh a1, 22(a0)`, `lw v1, 40(a0)`), and dividing the result
      by fifteen. **The count of quads in a pickup burst is computed from the item's own geometry, not
      constant**, and `q2_fx_preset` has a constant `count`.

      So this is not a missing table entry, it is a shape the preset structure cannot express — which is why
      it was right not to borrow one of the other seven. Adding it means either a variable-count spawn path
      or a stated approximation, and both are choices worth making deliberately rather than in passing.

      The counter itself decodes cleanly, so the next attempt starts from the arithmetic rather than the
      disassembly. `0x8006D6AC`:

          if (!a0) return 0;
          n = (s16)a0->[0x16];            ; a record count
          if (!n) return 0;
          p = a0->[0x28] + n * 8;         ; one past the end of an 8-byte-record table
          total = 0;
          do { p -= 8; total += (u8)p[3]; } while (--n);
          return total;

      A sum of the byte at `+3` of each of `n` eight-byte records, and the caller divides it by fifteen.

      **The structure is the MODEL**, and `model.h` already had every field: `0x16` is `num_parts`, `0x28` is
      `ofs_parts`, and the header's own note records that `ofs_faces - ofs_parts == 8 * num_parts` — 8-byte
      part records. Within one, `num_faces` is at `+0`, `vert_base` at `+2` and **`num_verts` at `+3`**. So
      `0x8006D6AC` totals the model's VERTICES across its parts, and a pickup burst throws

          count = (sum of every part's num_verts) / 15

      quads, from ramp 10 and ramp 0, at size 6144, area 0. A bigger item bursts bigger, in proportion to its
      mesh.

      No preset entry is needed: `q2_fx_group_spawn` already takes an explicit count, ramps, life, size and
      area, so the variable count goes straight in.

      **The last two arguments are read and it is implemented.** The wrapper's fifth argument is zero, which
      selects the spawner's second branch at `0x8005AC34`: **life 32**, and a velocity per component of

          v = ((rand() - 16384) * 3) / 16384      ; truncating toward zero

      from the `sll 1 / addu / bgez +16383 / sra 14` at `0x8005AC4C` — a drift of plus or minus three. The
      other branch, taken when that argument is non-zero, is fifteen quads at life 10 with a `>> 10` spread;
      the pickup is not it.

      So a pickup now bursts: ramp 10 and ramp 0, size 6144, area 0, life 32, `sum(part.num_verts) / 15`
      quads. The burst event carries the item's `model_index` because the game half has no bank to count
      with and the client does.

      Measured, and the early returns are counted rather than silent: **JAIL2 draws 2 bursts** with no
      rejections. WASTE3 collects two items and draws none — both are `no model`, `model_index < 0`, which
      is the original's own first line: `0x8006D6AC` opens `if (a0 == 0) return 0`, so an item with no model
      throws no particles there either. The port agrees with it by construction rather than by accident.

      BASE0 is byte-identical, so nothing that was not collecting an item changed.

      **And the light event is answered too: no item on this disc glows.** `item.c:846` raises it for any
      item whose flags carry `Q2_ITEM_GLOW` (bits 4-6, R/G/B), and across all 64 records of the item table the
      only flag that ever appears is `spin`. Not one has a glow bit. The event has never fired in a capture
      because there is nothing on the disc that would fire it.

      **The category this was filed under has since collapsed, and the collapse matters more than the
      category did.** It claimed three examples of "a path with no data behind it is not an incomplete
      port": the item glow, the Tank Commander's `tnk_` sounds, and the 69 rotation calls whose first object
      slot is -1. **Two of the three were wrong.** The rotation calls read their operands from the ZONE's
      Events chunk and most of them are live (#56); the `tnk_` sounds are on the disc as 63 VAG entries.
      Only the item glow survives, and it survives because it was checked by enumerating all 64 item records
      rather than by failing to find something.

      So the rule to keep is the narrower one: **"I looked and found nothing" is only evidence if you can
      say what you looked at and that it could have contained the answer.** Both refuted claims failed that
      test — one searched a summary with no names in it, the other read a buffer the engine writes -1 into. The port has seven — explosion, blood, BFG,
      gib, scripted, spark and laser end — and none of them is the pickup burst. Choosing one would invent
      an effect rather than reconstruct it, so nothing is drawn yet. `0x8005B6C0` is the original's, and
      reading its particle table is what this needs; the event carries the position and the glow colour
      already, so only the preset is missing.

      **And the "the light event has not fired in any capture, so the transient path is untested" line was
      also a statement about this port, not about the disc.** `q2_light_add_dynamic` transcribes
      `0x80075C34`, and asking who calls it:

          800288C8  80028E6C  8002A868  80031048  80031268  80048228  800482A0
          8004AD20  8004B8A0  8004CA14  8004CDE4  800586D0  800597C0  8005A760
          800648B8

      **Fifteen call sites**, spread from the script area through the item and weapon code to `0x8005A760`
      — which is also one of the twenty-three writers of `entity+0x100`. This port raises the light event
      from exactly ONE place, `item.c:858`, for glowing items, and no item on this disc glows. So the
      transient path is not untestable: **fourteen of the fifteen things that would exercise it are simply
      not reconstructed yet**, and their addresses are listed above.

      That is the third claim in a row of the form "the disc cannot exercise this" that turned out to mean
      "this port only implements one way in". Dynamic lighting moves off the blocked list and onto the work
      list, with a fifteen-entry starting point.

      **And the first of the fifteen is read.** `0x80048228` builds its arguments out of globals rather than
      computing them:

          80048204  lhu  a2, 2(v0)        ; v0 = 0x800AE958
          80048208  lhu  v0, -5800(v1)
          80048214  or   a2, v0, a2       ; a2 = lo | (hi << 16)
          8004821C  lhu  a3, 2(v0)        ; v0 = 0x800AE95C
          80048228  jal  0x80075C34
          80048230  lhu  v0, -54(s3)      ; then test bit 0x10...
          8004824C  lbu  v0, 1(a2)        ; ...and read RGB at 0x800AE954

      So there is a **preset table**, and its bytes decode:

          0x800AE954   FF 64 4B      RGB (255, 100, 75)   -- warm orange
          0x800AE958   2C 01 20 03   two u16: 300 and 800  -- inner / outer radius
          0x800AE95C   00 00 00 00
          0x800AE960   20 03 40 06   two u16: 800 and 1600
          0x800AE968   FF FF 00      RGB (255, 255, 0)    -- yellow
          0x800AE96C   20 20 20      RGB ( 32,  32,  32)  -- dim grey

      A colour triple and a packed `inner | outer << 16` radius pair, in a run. That is the piece every one
      of the fifteen sites will need, so it is worth more than any single site: **the presets are data at
      `0x800AE954`, not constants in code.** The second call, `0x800482A0`, is the same shape behind a
      `& 0x10` flag test — one light unconditionally and a second when the bit is set.

      **The first of the fifteen is now reconstructed.** `0x80047C6C` is the PROJECTILE sweep — so what that
      light is, is a bolt lighting the world it flies through. Every live projectile raises the light event
      with the preset above (`simcombat.c`), and the client feeds it to the light world instead of dropping
      it.

      **And doing that found a second bug that had nothing to do with projectiles.** The first measurement
      read `16 lights added, 481 dropped`: sixteen is the engine's ceiling, and the list was filling once
      and never draining. `q2_light_world_begin_frame()` — a transcription of `0x80075B94`, which empties the
      runtime lists at the top of every frame — **existed in this port and was called from nowhere.** With
      it wired in:

          BASE3   16 added, 481 dropped  ->  497 added, 0 dropped
          JAIL2                          ->  496 added, 0 dropped

      So any future dynamic light would have been silently capped at sixteen for the whole level. That is
      worth more than the projectile light itself, and it was only visible because the counter reported
      added and dropped separately rather than "lights: 16".

      **The second projectile light is read too**, and it is a wider halo rather than a different effect:

          800482A0  a1 = 0x800AE954 packed r | g<<8 | b<<16 | a<<24   -- the SAME colour
                    a2 = 0x800AE960 packed lo | hi<<16                -- 800 and 1600
                    a3 = 0x800AE95C                                   -- 0, as the first call

      So bit 0x10 buys a second light of the same warm orange at inner 800 / outer 1600, around the first
      light's 300 / 800. Both constants are defined in `projectile.h` and **neither is raised**, because the
      gate — which projectiles carry bit 0x10 in the halfword at `s3-54` — is not read. Raising it for every
      projectile would double the light on bolts the original leaves alone, so it waits for the flag.

      **The gate is read, and it never opens.** `s3 = record + 88`, so the tested halfword is at
      **record+0x22**; `0x8004D7BC` writes it from `s1`, which `0x8004D714` loads as a stack argument, so the
      flags are the spawner's fifth parameter. Its three callers pass **11, 14 and 11** — `0b01011`,
      `0b01110`, `0b01011` — and **bit 0x10 is clear in every one**. The second light does not fire for any
      projectile spawned through that function, so raising it would have been wrong, and this port is right
      by measurement rather than by caution.

      **The preset region is dumped, and it is bigger than the projectile's corner of it.** Two more sites
      read from further in — `0x8004AD20` and `0x8004B8A0` both take `a3` from `0x800AE994` and `a2` from
      `0x800AE990` / `0x800AE9C0`. The region `0x800AE950..0x800AE9D4`, as bytes and as u16 pairs:

          800AE954  FF 64 4B    rgb(255,100, 75)      800AE98C  FF FF 00  rgb(255,255,  0)
          800AE958  2C 01 20 03   u16   300   800     800AE990  D0 07 E8 03  u16 2000 1000
          800AE960  20 03 40 06   u16   800  1600     800AE9A8  7F 7F 7F  rgb(127,127,127)
          800AE968  FF FF 00    rgb(255,255,  0)      800AE9B0  2C 01 00 00  u16  300    0
          800AE96C  20 20 20    rgb( 32, 32, 32)      800AE9BC  00 FF 00  rgb(  0,255,  0)
          800AE970  C0 FF 40    rgb(192,255, 64)      800AE9C0  E8 03 78 05  u16 1000 1400
          800AE980  50 00 CE FF                       800AE9D4  C8 64 64  rgb(200,100,100)

      Warm orange for the projectile, yellow, white, green, a pale red — a palette of light presets with
      radius pairs beside them, which is what every remaining site will index.

      **One thing NOT to assume from this**: that `lo` is always the inner radius. The projectile's
      `300, 800` ascends and `0x800AE990`'s `2000, 1000` descends, so the packing order is only established
      for the site whose code was actually read. Each site's own `a2`/`a3` construction has to be read the
      same way `0x80048228`'s was.

      **The second site is identified: `0x8004AC4C` is the ROCKET.** It is referenced only as a materialised
      constant, never called — an entity think pointer — and the code that installs it names it beyond doubt:

          8004B030  addiu v0, zero, 20000    ; rocket lifetime, entity+0xF4
          8004B034  sh    v0, 244(s0)
          8004B03C  addiu v0, v0, -21428     ; 0x8004AC4C
          8004B040  sw    v0, 60(s0)         ; installed as the entity's think at +0x3C

      Its light, at `0x8004AD20`, is the same call shape as the projectile's but sources differently:

          a2 = 0x800AE990 packed lo | hi<<16   -- 2000 and 1000
          a3 = 0x800AE994                      -- 0
          a1 = FOUR STACK BYTES at sp+32..35   -- NOT the preset table

      So the rocket takes its radii from the palette and its **colour from somewhere computed** — the light
      varies per call, which is what a rocket whose trail dims would look like. Reading it means finding what
      fills `sp+32..35`, not reading another preset.

      **The packing question is settled by reading `0x80075C34` itself**, which is where it should have been
      answered first instead of by comparing two call sites:

          80075C3C  sw   a2, 8(sp)          80075C54  sw   a3, 12(sp)
          80075CD0  lh   v0, 8(sp)          80075C70  lhu  v0, 12(sp)
          80075CD8  mult v0, v0             80075C78  andi v0, v0, 0x7    -- 3 bits
          80075CE4  sw   t1, 20(a0)         80075C7C  sll  v0, v0, 11
          80075CE8  lh   v0, 10(sp)         80075C8C  lhu  v0, 14(sp)
          80075CF0  mult v0, v0             80075C94  andi v0, v0, 0x3    -- 2 bits

      **`a2` packs the two radii — both halves sign-extended and SQUARED into the light record — and `a3` is
      not a radius pair at all**: its low half is a 3-bit `style` and its high half a 2-bit `size_shift`,
      which is exactly the signature `q2_light_add_dynamic()` already has. Every site passing `a3 = 0` is
      passing style 0, size 0, and the earlier note listing `a3` beside `a2` as though both were radii was
      loose.

      So the projectile's `300, 800` is inner 300 / outer 800 — what this port implemented — and the second
      light's `800, 1600` likewise. **The rocket's `2000, 1000` really is inner > outer**, and that is now a
      property of the data rather than an ambiguity in the reading. It is still a reason not to wire the
      rocket blind, and checking it sharpens the anomaly rather than dissolving it.

      **The record layout confirms which half is which.** `formats/entity.h` documents the light record from
      the map data: `0x14 u32 inner_radius_sq`, `0x18 u32 radius_sq`. The engine writes `a2`'s LOW half
      squared to +20 and its HIGH half squared to +24, so **low is inner, high is outer** — the projectile's
      300/800 is inner 300, outer 800, exactly as implemented.

      That makes the rocket's 2000/1000 an inner radius larger than its outer, and the same doc records that
      `inner_radius_sq` is **always <= radius_sq across all 7,814 static lights on the disc**. The rocket's
      pair violates an invariant every static light obeys.

      Three readings survive and none is chosen: the halfwords at `0x800AE990` and `0x800AE992` may be
      independent globals rather than a pair; the runtime append may not honour the static invariant; or that
      site is still misread. `q2_light_atten` computes `den = outer - inner` in UNSIGNED arithmetic, so
      inner > outer underflows to a huge denominator and the light comes out at essentially zero — it would
      not crash, it would silently do nothing.

      **All three readings are now settled, and the answer is that the port is already faithful.**

      *Independent globals?* No. Every reference to `0x800AE990` and `0x800AE958` computes the base with an
      `addiu` and reads `+0` and `+2` from it; the `+2` halfword has **no independent reference anywhere in
      the image**. They are pairs.

      *Misread site?* No. The rocket's packing is instruction-for-instruction identical to the projectile's —
      `lhu` the high half at `+2`, `lhu` the low half at `+0`, `sll 16`, `or`. The raw words are
      `0x0320012C` (300, 800) and `0x03E807D0` (2000, 1000).

      *Does the runtime append honour the invariant?* It does not have to, because **the engine's own
      attenuation underflows exactly as this port's does.** `0x80076040`:

          80076044  lw   v0, 24(a0)       ; radius_sq  (outer)
          80076090  sltu v0, v1, a1       ; dist >= outer -> return 0
          80076098  subu v1, a1, v1       ; num = outer - dist
          8007609C  lw   v0, 20(a0)       ; inner_radius_sq
          800760B0  subu a0, a1, v0       ; den = outer - inner   <- UNSIGNED
          800760D8  beq  a0, zero, ...    ; only exact zero is special-cased

      Both scaled by the same factor, so `inner > outer` survives scaling and `den` wraps to a huge unsigned
      value on real hardware just as it would here. **The rocket's dynamic light contributes essentially
      nothing in the original game.** Not wiring it is not an omission — it is the same result, reached
      without adding code that appears to do something.

      That is worth more than the light would have been: one of the fifteen sites is now known to need no
      reconstruction at all.

      **Every site mapped to its presets, in one sweep.** Scanning back from each `jal 0x80075C34` for the
      globals it reads:

          800288C8  78E                        8004AD20  990 992 994 996     rocket
          80028E6C  799 79A 79B 79C 79E        8004B8A0  9BC..9C2 994 996
          8002A868  78C 78E                    8004CA14  9D4 9D6 9D7 994 996
          80031048  7D2                        8004CDE4  9D4 9D6 9D7 994 996
          80031268  7D4..7DA                   800586D0  AAC..AB6
          80048228  954..95E   projectile      800597C0  AB4 AB6
          800482A0  954..962   projectile 2nd  8005A760  AD4..AD7 AB4 AB6
                                               800648B8  B28 B2A B2C B2E

      (all `0x800AE___`.) So the palette spans `0x800AE78C..0x800AEB2E`, far wider than the projectile's
      corner, and several sites SHARE entries — `0x8004CA14` and `0x8004CDE4` read exactly the same set, and
      `0x800AE994/996` is the `a3` (style, size) that five different sites pass.

      Reading the radius pairs where they resolve:

          800AE958   300  800   projectile        live
          800AE960   800 1600   projectile 2nd    live, but its 0x10 gate is shut
          800AE990  2000 1000   rocket            INNER > OUTER -- no-op
          800AE9C0  1000 1400   0x8004B8A0        live

      **Only the rocket has the inverted pair.** The others are ordinary, so the no-op result does not
      generalise — but each site now has its preset addresses written down rather than needing its own hunt.

      **`0x8004B2B4` is the BFG BLAST, and its light is wired.** The spawner that installs it at `+0x3C`
      materialises the string `"BFGBlast"` (`0x800ACBBC`) two instructions earlier, so the identity is read,
      not inferred. Its light at `0x8004B8A0` takes RGB from `0x800AE9BC` and radii from `0x800AE9C0`:

          800AE9BC   00 FF 00      rgb(0, 255, 0)   green
          800AE9C0   E8 03 78 05   u16 1000, 1400   inner, outer

      A wide green glow against the small warm one every other bolt carries. Each kind's own think adds its
      own light in the original, so this **replaces** the generic preset for a BFG rather than adding to it —
      doubling them would light a BFG twice.

      Three of the fifteen sites are now settled: the projectile wired, the rocket proven a no-op, the BFG
      wired.

      **Two more identified, and the route generalises.** `0x8004CA14` and `0x8004CDE4` read the same
      presets, and both sit inside functions that are entries in a table at `0x8009D704`:

          8009D704  8004EB08     8009D718  8004CA9C  <- 0x8004CDE4 is in here
          8009D708  8004BFBC     8009D71C  8004EBDC
          8009D70C  8004C1C0     8009D720  8004CE18
          8009D710  8004C488     8009D724  8004D038
          8009D714  8004C744  <- 0x8004CA14 is in here
          ...       8009D730  8004EB10

      Twelve entries, and `weapon.h` already names them: index 11 is the BFG at `0x8004EB10`, which pins the
      base. So `0x8004CA14` belongs to the **Machinegun** (id 4) and `0x8004CDE4` to the **Chaingun** (id 5).
      **These are muzzle flashes.** Their colour is `0x800AE9D4` — `C8 64 64`, `rgb(200, 100, 100)`, a pale
      red.

      Their radii are not from the palette — they are **drawn**, and the draw is now read:

          8004C970  jal  0x80089E28        ; BIOS rand(), 0..32767 -> v0
          8004C978  sll/addu/sll/addu/sll  ; r * 100
          8004C98C  sra  s0, s0, 15
          8004C994  addiu s0, s0, 250      ; inner = ((r*100)>>15) + 250   250..349
          8004C998  sll/addu/sll/addu/sll  ; r * 200
          8004C9AC  sra  v1, v1, 15
          8004C9C0  addiu v1, v1, 700      ; outer = ((r*200)>>15) + 700   700..899

      **One draw feeds both**, so a shot's inner and outer always move together. And since `q2_rng_next` is
      the BIOS generator bit for bit, this reproduces the console's sequence rather than approximating it.

      **Wired**: `q2_weapon_muzzle_light()` in `weapon.c`, raised from the fire path for weapon ids 4 and 5
      only. The draw happens AFTER the shot so it cannot perturb the spread sequence.

      Proving it needed a test rather than a screenshot: a passive capture never fires, and deathmatch bots
      fired 0 shots in 500 frames. `tests/light` now recomputes both radii the long way — `((r<<1)+r)<<3`,
      `+r`, `<<2` — and checks the port's multiply agrees at 0, 1, 16384 and 32767, plus the documented
      endpoints 250/349 and 700/899.

      **Five of fifteen are now named**: projectile, rocket, BFG, machinegun, chaingun.

      **And the remaining ten are mostly SCRIPT.** Finding each site's enclosing function and asking what
      references it puts `0x800287A0` in a second table, at `0x8009B930`, whose records carry their names
      inline:

          8009B930  8002E640  "FLKLIGHT"
          8009B940  800287A0  "SHOOTTHEN"     <- WRONG: 0x800288C8 is FLKLIGHT's, see below.
                                                 The table reading was off by one record.

      That is the **UserFuncs command table**, and `src/formats/userfuncs.h` already lists those two
      primitives adjacently in the same order — `Q2_UF_FLKLIGHT` then `Q2_UF_SHOOTTHEN` — so the table is
      identified by two independent agreements, not by one string.

      `userfuncs.c` even carries their operand layouts: `TIMEDLIGHT` 28 bytes / 5 operands, `FLKLIGHT`
      24 bytes / 4 operands. **What the port lacks is a handler**, which is exactly the state `SIMROT` was in
      before `q2_rotators_build` existed — a decoded command with no runtime behind it.

      **TIMEDLIGHT is implemented.** `client_event_call` now raises a light event when the script runs one:
      origin from +4 as three s32, `radius` from +18 **tripled** (the engine's own multiply, recorded in the
      operand table, not a choice), and the packed colour at +24 read low-byte-first as r, g, b.

      **FLKLIGHT is not handled yet, but the reason has changed.** Its on/off times are randomised as
      `((rand()*500)>>15)+400`, which was recorded here as needing "the engine's RNG stream" — and the port
      turns out to already have it.

      `0x80089E28` is `addiu t2, zero, 160; jr t2` with `t1 = 0x2F`: a jump to the PlayStation **BIOS
      A-function table**, entry 0x2F, which is `rand()`. The BIOS implements that as
      `x = x*0x41C64E6D + 0x3039; return (x>>16) & 0x7FFF` — and `q2_rng_next` in `weapon.c` uses
      `1103515245` and `12345`, which are those two constants in decimal. **The port's generator is the
      console's, bit for bit, given the same seed.** The comment there claimed "any full-period generator
      does", underselling what was already implemented; it is corrected.

      **FLKLIGHT is now implemented.** `q2_flklights` in `lighting.c` holds the phase the transient light
      event has no room for: a flicker is registered once by the script, starts lit, and turns over on the
      operand table's own durations —

          on  = ((rand() * 500) >> 15) + 400        400..899
          off = ((rand() * 500) >> 15) + 1000      1000..1499

      drawn from the same BIOS-identical generator, so the blink is the console's rather than a rhythm of
      this port's choosing.

      ~~Two behaviours are pinned by `tests/light`: re-entry does not stack, and the turn-over is a `while`
      rather than an `if`.~~ **All of that is deleted, because the phase it protected does not exist.**

      The exec at `0x800287A0` loops the primitive's objects, adds one dynamic light each, and returns at
      `0x8002890C`. No timer, no state, nothing that could turn a light off. What makes a flicker flicker is
      that **both radii are redrawn from `rand()` on every call**, so the same script record casts a
      different-sized light each time it runs. FLKLIGHT is a transient exactly like TIMEDLIGHT.

      Three commits to get here, and each intermediate one looked finished: implement the phase, correct its
      constants, then find the phase should not exist. **The tell was that the tests passed at every stage** —
      they pinned arithmetic that was correct throughout, while what was wrong was what the numbers were FOR.
      A test that checks a formula cannot check its purpose.

      **And the handlers are reachable, which a fly-through could not show.** Firing every trigger volume on
      the disc through the script runtime:

          script LIGHT calls in COMMON: 18 TIMEDLIGHT, 1 FLKLIGHT;
          the trigger sweep RUNS 6 of them

      So six of the nineteen are reached by a trigger alone — the handlers are not dead code, they are code
      a passive capture never provokes. The other thirteen sit behind conditions the sweep does not satisfy
      (a door opened, a counter reached), which is the same reason most rotation calls only fire in play.

      That distinction is worth keeping: **"0 in a capture" and "0 reachable" look identical in a
      screenshot**, and only the sweep separates them. It is the same question that turned "69 inert rotation
      calls" into "67 of 68 live" and "54 barren" into "one truncated item".

      **Another site named, and it belongs to a creature.** `0x80031268` sits inside `0x80031094`, which the
      module loader writes to import-table offset **0xA0** (`0x8007DBC0`, `sw v0, 160(s0)`). Censusing every
      import call the disc's creatures make finds eighteen distinct slots, and exactly one step calls this
      one:

          Gunner   [13] call(+A0)?

      So **import +0xA0 is the creature muzzle-flash light**, and the Gunner's think 13 is its only caller on
      the whole disc. Its colour is `0x800AE7D4` — `C8 64 64`, rgb(200, 100, 100) — **the same pale red as
      the player's machinegun flash**, which is a pleasing cross-check: two independently-read sites landing
      on one colour.

      ~~Its input is not a plain `rand()` draw — it carries a sign.~~ **Wrong, and reading four instructions
      further back settles it.** There are TWO `jal 0x80089E28` calls, not zero:

          800311B0  jal  0x80089E28       ; r1 = rand()
          800311B8  sll/addu/sll/addu     ; r1 * 25
          800311C8  sll  v0, v1, 3        ; r1 * 200
          800311DC  addiu v0, v0, 850     ; inner = ((r1*200)>>15) + 850    850..1049
          800311E0  jal  0x80089E28       ; r2 = rand()   (delay slot stores inner)
          800311E8  sll/addu/sll/addu     ; r2 * 25
          800311F8  sll  v0, v1, 4        ; r2 * 400
          80031220  addiu v0, v0, 1200    ; outer = ((r2*400)>>15) + 1200  1200..1599

      The `bgez ...; addiu 32767` around each shift is the **compiler's signed-divide idiom**, not evidence
      of a signed input — `rand()` returns 0..32767, so that arm is never taken. Reading it as a sign was the
      whole reason the previous entry gave for not wiring this, and it was a misreading of boilerplate.

      **Wired**: `q2_creature_muzzle_light()`, with `tests/light` pinning both endpoints and, importantly,
      that the two draws are independent — the creature flash uses TWO where the player's uses one, so an
      implementation that reused a single draw would produce correlated radii and look right.

      **And one more site named, which turns up the first non-zero style.** `0x800648B8` sits in
      `0x80064780`, which `model.h` already identifies as **the fixed-size effect renderer** — so this is an
      effect's own light. Its operands:

          800648AC  lhu  v0, 0x800AEB28   ; a2 = 300 | 800<<16   -- the projectile's radii exactly
          800648A8  lhu  a3, 0x800AEB2C   ; a3 = 1 | 3<<16

      `a3`'s halves are the 3-bit `style` and 2-bit `size_shift` (settled at `0x80075C70`), so this site
      passes **style 1, size_shift 3** — every other site examined passes 0, 0. It is the first evidence that
      those fields are used at all, and `formats/entity.h` records the static lights' `type` byte taking
      exactly five values `((n<<3)|7)` for n in 0..4 with "style semantics unknown". A dynamic light asking
      for style 1 is a second sighting of the same field, from a different direction.

      Not wired: the port's effect system would have to raise it, and which of the seven presets this
      renderer is drawing is not yet read.

      **And the style field's meaning follows from there.** Exactly one place in the image extracts bits
      11..13, and it is a five-way switch:

          80075720  srl  v0, a0, 14 ; andi 0x3     ; size_shift
          80075728  sllv a1, v1, v0                ; ...which SHIFTS a value left
          8007572C  srl  a0, a0, 11 ; andi 0x7     ; style
          80075738  beq a0, 2 -> t1 = 0x800A2014
          80075748  beq a0, 1 -> t1 = 0x800A1FDC
          8007575C  beq a0, 3 -> t1 = 0x800A2024
          80075764  beq a0, 4 -> t1 = 0x800A203C
                    default   -> t1 unchanged

      **Five cases, 0..4** — precisely the five values `((n<<3)|7)` for n in 0..4 that `formats/entity.h`
      records for the static lights' `type` byte with "style semantics unknown". The static census gave the
      value set; this gives the dispatch. They are the same field reached two ways.

      Each style selects a TABLE, and the tables are runs of 8-byte records ending in a zero pair:

          0x800A1FDC  style 1   six records
          0x800A2014  style 2   one record      1, 4096, 4096, 4096
          0x800A2024  style 3   two records
          0x800A203C  style 4

      ~~The individual curves are not decoded yet.~~ **They were decoded before this session started.**
      `src/game/flare.c` has all four transcribed as `k_style1..k_style4`, at those exact addresses, with a
      header naming the four routines involved and noting that `q2psx-inspect lights` reads the same bytes
      back off the disc and compares them element by element.

      So `style` is a **LENS FLARE style**: each list is one `BURST` element plus a run of `DISC`s, and
      `size_shift` scales the whole flare. Style 2 is the core alone; style 4 is style 1 less one element.
      The values I read as an "animation curve" — 4096, 2048, 0x044C, -0x0ED8 — are element sizes and
      offsets along the centre-to-light line.

      What was actually missing was a POINTER: `formats/entity.h` said "style semantics are unknown" while
      `game/flare.c` had held the answer all along, and nothing connected the two. That note now points at
      the file. **The lesson is not the light: it is that "unknown" in one file is worth grepping the tree
      for before it is worth disassembling.** Four rounds of this session have now ended in a claim that was
      already answered somewhere in the repo or already wrong.

      **Applied immediately, and it places every remaining site with no new disassembly at all.** Grepping
      the tree for each unidentified function's address and its neighbours:

          0x800597C0  in 0x80059330  = THE ITEM THINK. `itemtable.h` names it outright, and `item.h`
                                       already documents this very light: a quarter of the pulse per lit
                                       channel (0x80059704), radius pulse + 100 (0x80059798), offsets
                                       -50 / -500. Implemented at `item.c:846` and correctly dormant,
                                       because no item on the disc carries a glow bit.
          0x8005A760  in 0x8005A600  = ENTITY code (entity.c cites 0x8005A9DC nearby) — and also one of the
                                       23 writers of `entity+0x100` found back in #51b.
          0x800586D0  in 0x80058638  = a DAMAGE EFFECT. `combat.h` documents `q2_mod_effect_timer` as
                                       spanning 0x800585A4..0x80058604 — "the damage-effect timer a mod
                                       arms" — and 0x80058638 is the next function after it. Its light is
                                       rgb(0, 255, 0) at 800 / 1300, read from 0x800AEAAC and 0x800AEAB0,
                                       with style 0. Green, and close to but not the same as the BFG
                                       blast's 1000 / 1400, so the two are separate effects rather than one
                                       shared preset. Its two callers, 0x8005B3C4 and 0x8005B46C, sit
                                       beside the shrink-and-vanish helper item.c cites at 0x8005B368.

                                       **Which damage effect is now read too.** The handler gates on one
                                       byte: `lbu v0, 753(s1)` is `entity+0x2F1`, which combat.h maps to
                                       `effect[1]`, and `sltiu v0, v0, 3` sends anything below three down a
                                       different arm. `Q2_MOD_ENERGY_BOLT` is the mod that arms slot 1, and
                                       it arms it with exactly **3** — so the green light is what an
                                       energy-bolt hit looks like on the tick it lands, and the `< 3` arm is
                                       that effect's tail. `q2_actor_energy_lit()` carries the predicate,
                                       with `tests/combat` pinning the boundary at 3 rather than at 0:
                                       a `> 0` reading would light the whole tail and look entirely
                                       reasonable doing it.

                                       **Raised** from the projectile tick in `simcombat.c` — the player's
                                       own actor and every world target, one light each while lit. Put
                                       there rather than at the damage site on purpose: the effect outlives
                                       the tick that armed it, so lighting it where the damage lands would
                                       give a one-frame flash where the original gives a burn.

                                       ~~The client pairs the inner radius off the outer the event
                                       carries.~~ **The event carries both now**, which is the fix that
                                       workaround was asking for. It held only the outer, and the client
                                       recovered the inner by matching against the presets it knew —
                                       800 -> 300, 1400 -> 1000, 1300 -> 800. That worked while every
                                       preset had a distinct outer and would have failed **silently** the
                                       first time two shared one: a light of the wrong size, on a path with
                                       no error to report. `q2_ent_light_at` takes `inner` now, `0` means
                                       the raiser had none to give, and every call site passes its own.
                                       Same counts before and after — 499 on BASE3, 499 on JAIL2 — because
                                       this changes what the code can express, not what it currently does.
          0x8002A868  in 0x8002A660  = GLASS's destruction light. `userfuncs.h` already documents the
                                       primitive — 16-byte item, object slot at +4, hit points at +6 — and
                                       notes that GLASS spawns its effect at 0x8002A384 before examining
                                       its damage argument. This light sits in that same path.

                                       Its radii are NOT a palette entry. `a2` is read from `gp+16840`
                                       (0x800B27C8), and that global is written a few instructions earlier
                                       in the same function:

                                           8002A7E4  addiu v1, zero, 1000
                                           8002A7E8  sh    v1, 16840(gp)   ; 0x800B27C8
                                           8002A7EC  addiu v1, zero, 2000
                                           8002A7F0  sh    v1, 16842(gp)   ; 0x800B27CA

                                       Two consecutive halfwords, which is exactly the `lo | hi<<16` pair
                                       the light reads — so **breaking glass lights the room at inner 1000,
                                       outer 2000**, the widest of any site read so far. The values are
                                       immediates in the glass path rather than data, which is why the
                                       palette sweep could not find them.

                                       Its colour IS a palette entry after all — `0x800AE7A8`, `FF 00 00`,
                                       **pure red**, with `0x800AE7AC` holding pure blue beside it. The
                                       earlier sweep missed it only because that scan looked back 22
                                       instructions from each `jal` and this load sits further out. So the
                                       palette runs wider than the 0x800AE954 corner first examined, and a
                                       red/blue pair sits near its start.

                                       **And the red is why the label above is withdrawn**: pure red is not
                                       what breaking glass looks like. Adjacency put this site in GLASS's
                                       neighbourhood and that was worth having; it did not name it, and
                                       calling it "GLASS's destruction light" was a step further than the
                                       evidence went.
          0x80031048  in 0x80030E74  = EFFECT code (effect.c cites 0x80030430), and it does not read one
                                       colour — it reads a **six-step fade ramp**, `0x800AE7BC..0x800AE7D0`:

                                           rgb(153, 54, 0)   orange
                                           rgb(122, 43, 0)
                                           rgb( 91, 33, 0)
                                           rgb( 61, 22, 0)
                                           rgb( 31, 11, 0)
                                           rgb(  0,  0, 0)   out

                                       Each step is roughly 0.8x the last, ending at black. So this light
                                       DIMS over six steps rather than holding a colour — an ember or an
                                       explosion cooling, and the first site found that animates its own
                                       colour rather than its radius.
          0x80028E6C  in 0x80028BDC  = the script command area, beside SHOOTTHEN

          0x800288C8  in 0x800287A0  = **FLKLIGHT, not SHOOTTHEN.** The colour bytes come from the item at
                                       +18/+19/+20 (`0x80028808`), which is FLKLIGHT's documented operand
                                       layout and not SHOOTTHEN's 8-byte one — so the earlier reading of
                                       the command table at 0x8009B930 was off by a record. Its parameters
                                       are still assembled on the stack:

                                           80028890  lbu  v0, 26(sp)      ; colour bytes at sp+24..27
                                           800288BC  lhu  v0, 16(sp)      ; a2 low  = inner
                                           80028894  lhu  a2, 18(sp)      ; a2 high = outer
                                           800288B8  lbu  v1, 27(sp)

                                       No palette entry, no immediate, no rand draw. The `78C / 78E` the
                                       sweep attributed to it are the `a3` (style, size) pair and nothing
                                       more. So a shootable trigger's flash is coloured and sized by
                                       whatever set up that frame, and reading it means finding the caller
                                       that fills sp+16..27 rather than reading another table.

      **Four ways a site gets its parameters**, which is the useful generalisation from all of this:

          a palette entry          the projectile, the BFG, the energy bolt, the effect ramp
          a rand() draw            both muzzle flashes, player and creature
          immediates -> globals    the red 1000/2000 site, written in its own function
          entirely off the stack   SHOOTTHEN

      A sweep that only reads the palette finds the first kind and misses three.

      **A caveat on that sweep, learned by it being wrong.** It scans back from each `jal` for annotated
      `0x800AE___` operands, and the annotation only appears when the disassembler can resolve the base
      register. `0x8002A868`'s colour load is `lbu a1, -6232(v0)` with `v0` materialised far earlier, so the
      sweep never saw `0x800AE7A8` — the entry that turned out to name that light. Widening the window from
      22 instructions to 60 found more (`0x80031048`'s ramp among them) but cannot fix that class of miss.
      **Absence from these lists is not evidence.**

      So the fifteen are no longer a list of addresses: **one is wired and dormant by design, one is the
      creature flash, two are the player's, one each for projectile, rocket, BFG and the effect renderer, and
      the rest sit in four named subsystems.** That map cost one `grep` per address, after several rounds of
      disassembly had produced less.

      How many exist, counted rather than grepped: **18 TIMEDLIGHT calls and 1 FLKLIGHT across COMMON's
      scripts, disc-wide.** A passive capture triggers none of them — they are script records fired by
      trigger volumes, exactly like the rotation calls — so the handler shows 0 in a fly-through and that is
      the correct number, not a failure.

      *A repeat of a mistake worth naming:* the first attempt to count these was
      `q2psx-inspect events <map> | grep -i timedlight`, which returned zero for every map. That command
      prints aggregate statistics and no primitive names, so the grep could not have found anything — the
      same invalid search that made the Tank Commander's sounds look absent (#60). The counter above is in
      `zonescript` now so the question has a real answer next time.

      So the shape of the remaining lighting work is now known: **not ten unrelated hunts, but the script
      light primitives, built the way the rotators were.** `0x80031094` being materialised at `0x8007DBBC`
      — inside the module loader — points the same way, since that is where the 71-slot import table is
      filled.

      Still not done: fourteen of the fifteen sites — but they share a dumped table now, and two of the
      fourteen are identified (rocket, and `0x8004B2B4` reached the same way).

---

## ⚠ Security note (carried forward, do not drop)

- [!] 38. **A STANDING CAUTION, not an open question — it is never going to be "resolved" and must not be
      counted as work outstanding.** A prior research pass reported that a fan wiki page about this game served content containing
      **instructions addressed to AI agents** (create files, transfer funds, insult the operator, terminate
      operations). That URL was **not** fetched during verification. **Treat it as hostile for any automated
      fetch**; if data from it is wanted, a human should open it in a browser.
      Consequently, all web-sourced claims in the release census — the existence of the NTSC SKU, its
      timestamps and track lengths, barcodes, magazine demo-disc serials, and the absence of a Japanese
      release — remain **unverified** and should be treated as moderate confidence at best.

- [x] 56. **The 69 "inert" rotation calls are not inert — the operand is read from a SECOND buffer.**
      `q2_rotators_call` reads a SIMROT's four object slots from `item + 12` in COMMON's Events chunk, finds
      -1 in all four on 69 of 95 calls disc-wide, and builds nothing. The engine does not do that. At
      `0x800285CC` it sets up **two** cursors:

          800285CC  addiu s1, s5, 12       ; s1 = item + 12, in the chunk at gp+372
          800285BC  lw    v0, 372(gp)      ; base A
          800285C0  lw    a0, 376(gp)      ; base B
          800285F0  subu  v0, s1, v0       ; s1 - baseA  = the offset
          800285F4  addu  s2, v0, a0       ; s2 = that offset, rebased into base B
          8002861C  sh    s6, 0(s1)        ; STAMP -1 into buffer A
          80028620  lh    v0, 0(s2)        ; READ the slot from buffer B
          80028628  bltz  v0, 0x8002875C   ; negative -> skip this slot

      So the buffer we parse is the one the engine **writes -1 into**, and the operand it actually reads
      lives at the same offset in another. Reading only buffer A is guaranteed to see -1 wherever the game
      has run, which is exactly the shape of the 69.

      **Tested against the disc.** Taking each call whose COMMON slots are all -1 and reading the same offset
      in that map's ZONE Events chunks:

          empty in COMMON, a ZONE reaches that offset : 15
          ...and the ZONE has a non-negative slot     : 15
          zone slots examined 236, non-negative 16 (6.8%)

      **15 of 15**, against a 6.8% per-slot background. Coincidence would put roughly a quarter of the
      fifteen over the line, not all of them, and the 16 non-negative slots are spread one per call rather
      than clumped. `no object` falls 69 -> 54 and `usable` rises 26 -> 41 on that one change. The remaining
      39 are untestable here only because no zone chunk on those maps reaches the offset.

      **ANSWERED, and the header comment in `cmd_zonescript.c` was wrong.** That comment argued the engine
      never loads a zone's Events chunk, listing the zone loader's chunks as "AreaConx, CastList, CreAIBin,
      CreAIRel, MapMod, MapNames, Points, Scene, SortData, SpaceLights and the two collision hulls — and
      `Events` is not among them." It is among them. The zone loader's own name run reads:

          CastList   Events   CreAIBin   CreAIRel   SecondaryCol   SecondaryRem
          MapNames   SpaceLights   SortData   Scene   MapMod   Points   AreaConx

      `0x8007C14C` loads `0x800AD480 "Events"` and looks it up with base `*(gp+18856)` — the ZONE file's
      base, the same base the CastList and CreAIBin lookups beside it use — then stores the result at
      `gp+376` (`0x8007C234`). There are two Events LOADERS, not one: COMMON's at `0x8007AC30` into `gp+372`,
      and the zone's here. The old claim of "exactly two references to the string" was right about the count
      and wrong about what the second one does.

      **Fixed and measured.** `q2_rotators_set_operand_source()` rebases each slot read the way
      `0x800285F4` does. Disc-wide, through `zonescript`:

          rotators built   26 -> 62
          rotation steps   17 -> 40
          tick-moves        8 -> 399
          rotators turned  13 -> 36

      In the client, per zone: **JAIL2 zone 1 goes 0 -> 10 rotators**, JAIL3 zone 1 goes 0 -> 2, WASTE2
      zone 1 goes 0 -> 1. Zones whose Events chunk is byte-identical to COMMON's (21 of 74) are unchanged,
      which is why the default zone of most maps looks the same and why this hid for so long.

      **And the remaining gap is ONE call, not 29.** "29 executed calls turn nothing" was itself an artefact
      of measuring one zone at a time: the engine holds one zone resident, so a call that turns nothing with
      zone 0 loaded may turn geometry with zone 3. Sweeping every zone each map ships and asking whether a
      call site turns something under ANY of them:

          distinct rotation CALL sites the script reaches : 68
            turn something under SOME zone : 67
            barren under EVERY zone        : 1

      The one holdout is named — **WASTE2, Events+774** — and it turns out not to be a gap in the port at
      all. Dumping its slots in every buffer:

          COMMON (792 bytes) slots: (offset past the end)
          ZONE0  (792 bytes) slots: (offset past the end)
          ZONE1  (792 bytes) slots: (offset past the end)
          ZONE2  (792 bytes) slots: (offset past the end)
          ZONE3  (792 bytes) slots: (offset past the end)

      WASTE2's Events chunk is 792 bytes in COMMON and in all four zones. The item starts at 772 and a
      SIMROT's operands run to 796. **The item is truncated on the disc**: it declares a length that
      overruns its own chunk by four bytes, so there is no buffer anywhere holding its object slots. The
      engine reading it would read past the chunk into whatever follows.

      So the rotation question closes completely: **68 of 68 reachable call sites accounted for — 67 turn,
      and the 68th cannot, because the data for it is not on the disc.** Note this is invisible to the
      `too short : 0` counter, which checks the item's declared length and not whether the chunk can hold it.

      The `cmd_zonescript.c` header that asserted the opposite of all this has been rewritten in place rather
      than left to mislead the next pass.

      **What the remaining gap looked like before that sweep, measured rather than estimated.** "54 calls name no object" is
      not the same claim as "54 rotations are missing", because a call only matters if the script runs it.
      Counting the rotation primitives the trigger sweep actually reaches:

          rotation CALLs the script RUNS : 68, of which turn nothing : 29

      So the honest figure is **29 executed rotation calls that still turn nothing**, not 54 — and 39 of the
      68 now do turn something, against 13 before the fix. Of the 54 empty calls, 39 could not even be tested
      against a zone chunk because COMMON's Events chunk is larger than the zone's at that offset; the engine
      rebasing into a shorter buffer there would read past its end, so either those calls never fire or the
      rebase has a bound this pass has not found.

      One trap worth recording: `q2_rotators_build` memsets the set, so setting the operand source before
      building — the only order that works, since the build does the reading — was silently discarded until
      the build was taught to carry those three fields across the memset. The creature binds lost a whole
      pass to exactly this shape of bug.

      ~~What is NOT established is what buffer B is.~~ This file's own header argues the engine never loads
      a zone's Events chunk — one "Events" string, matched only by COMMON's loader at `0x8007AC30` storing
      into `gp+372`. `gp+376` is written at `0x8007C234` from a stack slot and cleared beside `gp+372` at
      teardown, so it is a peer of the events pointer, but its identity is unproven. The zone chunk is a
      CANDIDATE that fits the data, not a demonstration. **Find what writes `gp+376`** — that is the whole
      question, and it decides whether this port should be reading zone Events after all or some pristine
      second copy of COMMON's.

- [x] 57. **The modules name moves this port's decoder never reaches — and the first count of them was junk. CLOSED at THREE, all the Soldier's.**
      Chasing the three unnamed moves from 51h turned up a namer bug and then a measurement trap.

      **The bug:** `q2_creature_move_names` matched a name record to a move and then `break`, so one record
      could name only ONE move. A module may list the same range in two callback slots — the Arachner has
      16-24 twice — and the duplicate came out unnamed, reading as "the disc does not name this" when the
      name was right there. Fixed; disc-wide naming goes **89 -> 90 of 101**.

      **The trap:** the obvious next question is the other direction — name records that no decoded move
      claims, which would mean the module knows behaviour the decoder never found. The first counter said
      **241**, and the number was worthless. Printing them showed what they were:

          "Invalid Creature"  "Creature Interfa"  "d Creature Inter"  "eature Interface"

      `name_slot_ok()` accepts any 4-byte-aligned window that looks name-like, so the scan was walking
      straight through the modules' error strings and counting every misaligned slice of them. Requiring the
      record's frame pair to be ordered and small (`first <= last`, `last <= 1024`) — which a slice of
      English text will not satisfy — drops it to **42**, and real names appear:

          Soldier   "Run"  "Fire 1 Ready"  "Fire 1 Aim"
          others    "idle1"  "death"  "Walk"  "Pain 2"  "Stand"  "Attack2"

      So the honest picture is two-sided: **11 decoded moves the module does not name, and at most 42 named
      records no decoded move claims.** The Soldier naming a "Run" the decoder never installed is the
      interesting one — it says the gap is in the decode, not on the disc.

      42 is still an upper bound, not a count: junk like `"! @"` and `"%(E"` survives the tighter filter.
      Tightening it further, or reaching those moves in the decoder, is the next step. **Do not quote 42 as
      a finding** — quote the named examples, which are individually checkable.

      **CLOSED, and the answer came from the decoder rather than from the filter.** #58's merge-splitting and
      #59's named-but-unreached installer between them took the count from 42 to **three**, and all three are
      the Soldier's:

          30-31  "Death6"
          32-35  "Fire 3 Aim"
          36-38  "Fire 3 Shoot"

      A structural test was tried on top and is NOT kept: requiring a candidate record to have a neighbour at
      the 20-byte table stride — which is what actually separates a record from a lucky window — changes the
      count by zero. The text filter was already sufficient on this disc, and adding a rule that rejects
      nothing is code with no evidence behind it.

      **What is peculiar about the three is the fourth.** `Fire 3 Done` (39-44) IS decoded and installed, so
      the Soldier's third firing sequence is not absent from the module — only its aim and its shoot are.
      Scanning the whole image word-aligned for a move record `{u32 first, u32 last, ...}` carrying 30-31,
      32-35 or 36-38 finds **zero candidates**, where every range the installer does place finds one. So the
      module names an aim and a shoot it carries no record for, and carries a record for the recovery that
      follows them.

      Two readings fit and the disc does not separate them: the pair was cut and the name table kept, or
      their records are built at run time by a callback rather than sitting in the image. What can be said
      is that the residue is three records, they are named, they are all one creature's, and the decoder is
      not what is missing them — the same shape as the Tank Commander's five `tnk_` sounds that appear in no
      sound bank (#104).

- [x] 58. **Two of the four 3:1 exceptions are a DECODER bug: it merges adjacent moves into one.**
      Printing the unclaimed name records WITH their frame ranges — rather than just their names — answers
      51h's residue outright. The Soldier's module names these, and the decoder has no move for any of them:

            0-0    "Run"            15-19  "Fire 2 Ready"
            1-5    "Fire 1 Ready"   20-26  "Fire 2 Aim"
            6-8    "Fire 1 Aim"     27-29  "Fire 2 Shoot"
            9-11   "Fire 1 Shoot"  215-224 "Fire 2 Done"
           12-14   "Fire 1 Done"   225-247 "Walk 1 Loop"

      Now compare the decoder's two anomalous moves. Its **0-11** spans `Run` + `Fire 1 Ready` +
      `Fire 1 Aim` + `Fire 1 Shoot`. Its **215-247** spans `Fire 2 Done` + `Walk 1 Loop`. **The decoder is
      merging adjacent moves into one**, and a merged move's length is a sum, so three times it matches no
      single clip — which is exactly and only how those two showed up as anomalies.

      Split at the module's own boundaries, every piece lands:

          0-0    "Run"           1 frame  ->   3  matches a clip
          1-5    "Fire 1 Ready"  5 frames ->  15  matches a clip
          6-8    "Fire 1 Aim"    3 frames ->   9  matches a clip
          9-11   "Fire 1 Shoot"  3 frames ->   9  matches a clip
        215-224  "Fire 2 Done"  10 frames ->  30  matches a clip
        225-247  "Walk 1 Loop"  23 frames ->  69  matches a clip

      **Six for six.** So the 3:1 rule was never violated here; the port's move boundaries were wrong. That
      also explains why the "1x instead of 3x" coincidence looked tempting — a merge of a 1-frame move onto
      others shifts the length by exactly the wrong amount to fake a different ratio.

      **The Arachner's two remain.** Its unclaimed records are `"udeath"` 0-0, `"Pain 2"` 78-93 and
      `"Stand"` 65-77 — none overlapping its anomalous 16-24 `"Sway"` or 25-33, so merging does not account
      for them. Those two are still open, and they are now the ONLY moves on the disc that resist the rule.

      **DONE — `split_merged_moves()` in `creature.c`.** Every decoded move is checked against the module's
      own name records, and any strict interior range becomes a move of its own; the parent shrinks to the
      span before the earliest piece that follows it, so nothing that already refers to a move index changes
      meaning. A move the table does not subdivide is untouched.

          moves decoded        101 -> 108
          named                 90 -> 100
          unnamed               11 ->   8
          unclaimed records     42 ->  28

      And the rule itself, re-tested on the corrected boundaries:

          3:1 rule after splitting: 105 of 108 (97%)
            miss: Arachner 16-24 (9 frames wants 27)
            miss: Arachner 16-24 (9 frames wants 27)   <- the same move, listed twice
            miss: Arachner 25-33 (9 frames wants 27)

      **The Soldier's two exceptions are gone**, exactly as the split predicted. What is left is two distinct
      Arachner moves out of 107 distinct on the disc — `"Sway"` and its unnamed neighbour, both 9 frames
      wanting a 27-frame clip the Arachner does not have. Its unclaimed records (`"udeath"` 0-0, `"Pain 2"`
      78-93, `"Stand"` 65-77) do not overlap either, so merging is not the answer for them. That is the whole
      remaining residue of the 51 series.

- [x] 59. **The Arachner's residue, accounted for clip by clip: two moves and two clips, one frame apart.**
      Its model has 13 clips and its module 11 decoded moves plus 2 the decoder never finds. Assigning each
      move to a clip by the 3:1 rule and striking that clip off:

          0-12    "Start Melee"  13 frames ->  39   clip
          16-24   "Sway"          9 frames ->  27   NO CLIP
          94-109  "Attack 1"     16 frames ->  48   clip
          130-132 "Rear"          3 frames ->   9   clip
          133-135 "Start Attack"  3 frames ->   9   clip
          40-45   "Pain 1"        6 frames ->  18   clip
          35-39   "Melee"         5 frames ->  15   clip
          110-129 "Attack 3"     20 frames ->  60   clip
          25-33   (unnamed)       9 frames ->  27   NO CLIP
          53-64   "Death 2"      12 frames ->  36   clip
          136-138 "Walk"          3 frames ->   9   clip

      Then the two the module NAMES but the decoder never reaches:

          78-93   "Pain 2"       16 frames ->  48   clip -- and it was unused
          65-77   "Stand"        13 frames ->  39   clip -- and it was unused

      **Both land on clips nothing else had claimed.** That is independent confirmation of the rule from a
      direction that could easily have failed, and it identified two real moves this port did not install.

      **Now installed — `add_named_but_unreached()` in `creature.c`.** The name record gives a range but not
      the move record's address, so the range is what to search for: a move record is
      `{u32 first; u32 last; u32 frames; u32 endfunc}`, and scanning word-aligned for a matching first/last
      pair finds it. `add_move()` then validates the candidate on exactly the terms it validates a
      callback-reached one — frames inside the image, every ai byte selecting a real verb — so a coincidental
      pair of words is rejected the same way any other bad candidate is. They enter with `via == -2`, distinct
      from `-1` ("reached only through another move's endfunc"), so nothing mistakes them for
      callback-installed behaviour.

          moves decoded    108 -> 115        named            100 -> 107
          unclaimed         28 ->  21        3:1 rule     105/108 -> 112/115

      The Arachner has a pain animation and a stand animation it did not have, and JAIL4 now resolves 9
      creature sounds where it resolved 6. It is not only the Arachner: the **Berserk** gains `"Attack2"`
      (96-109), 14 frames wanting 42, and 42 is one of its clips — so the new move satisfies the rule on
      arrival.

      **One regression, run down rather than left as a number.** Think indices went from 51 of 51 decoding to
      54 of 55. The single `?` is the **Berserk's think 1**:

          [ 1] move(8010157C)? sound(80101758)?

      ~~Its move and its sound are behind a branch the decoder will not follow.~~ **That was the wrong
      think.** `gated` marks a step as CONDITIONAL, not undecoded, and think 1 decodes fine. The index with
      no action is **think 7**, and disassembling it ends the question:

          80101008  03E00008  jr      ra
          8010100C  00000000  nop

      **It is an empty function.** The move `"Attack2"` calls a think that deliberately does nothing, and
      "this frame does nothing" is an answer the disc gives, not a failure to read it. There is no
      regression: the count fell because the census had no way to say "decoded, and the answer is nothing".

      `q2_creature_think_is_empty()` now distinguishes the two, and the census says so outright:

          54 of 55 think indices across the disc decode to an action; a ? marks one behind a branch
          1 of the remainder is an EMPTY function on the disc -- `jr ra` and nothing else

      The lesson is the one this file keeps relearning: **a counter that cannot express "nothing" reports
      absence as failure.** The same shape as `too short : 0` hiding WASTE2's truncated item.

      What is then left is exact: **two clips unclaimed, both 30 frames — ten AI frames each — and two moves
      unmatched, both nine AI frames.** Two leftovers on each side, differing by exactly one frame.

      ~~That points at an off-by-one in those two ranges.~~ **Tested and REFUTED the same session.** The
      move record is `{u32 first; u32 last; u32 frames; u32 endfunc}` read straight out of the module, so
      there is no arithmetic for the decoder to get wrong, and the frame arrays confirm it independently:

          ( 3) 801017DC end 00000000->00000000  16-24  -> 0*9  ai: 2*9
          ( 4) 80101808 end 00000000->00000000  16-24  -> 0*9  ai: 3*9
          (-1) 801018A4 end 80101010->80101738  25-33  -> 0*2 3 0*3 4 0*2  ai: 4*9

      Nine frame entries each, every one validated against the verb table before the move was accepted, and
      the name table independently agrees on 16-24. **Three sources say nine, so the spans are nine** and the
      tidy "two leftovers, one frame apart" reading is wrong.

      Also visible there: 16-24 is TWO distinct move records at different addresses (`801017DC` via callback
      3, `80101808` via callback 4) that happen to share a range — not one move listed twice, as earlier
      entries in this file assumed.

      So the residue is real and stands: **the Arachner has two 9-frame moves wanting a 27-frame clip it does
      not have, and two unused 30-frame clips nothing claims.** No off-by-one, no merge, no variant model —
      its model is the only Arachner on the disc. This is where the 51 series ends for now, with the anomaly
      stated precisely rather than dressed in an explanation that does not survive.

- [x] 60. **The Arachner's last two: the loop hypothesis tested and refuted.**
      Reading the per-move lines gave a promising shape — the two 16-24 records are `via 3` and `via 4`, the
      Arachner's WALK and RUN, and both have `end 00000000`, meaning they loop. A looping clip plausibly
      carries one extra frame to close the cycle, which would turn 9 AI frames into 10 clip frames and land
      exactly on the two unused 30-frame clips.

      Tested across the disc, splitting every move by whether it loops:

          ends  moves:  88   n*3 matches  87   (n+1)*3 matches   1   neither  0
          loop  moves:  27   n*3 matches  25   (n+1)*3 matches   2   neither  0

      **Refuted.** Twenty-five of twenty-seven looping moves match `n*3` exactly, so looping is not what
      distinguishes the Arachner's two. And the third anomaly (25-33) is a TERMINATING move, so whatever the
      property is, it is not "loops behave differently".

      What is true is that all three anomalies match `(n+1)*3`. On the Arachner's clip list that fallback
      lands 14% of the time for an arbitrary length — the same rate as `n*3` — so two distinct cases both
      hitting it is roughly a 2% coincidence. Suggestive, not a finding, and **no mechanism has been found
      that separates those two moves from the 113 that fit.** Recorded as the residue it is.

      **CLOSED, by asking what the residue can REACH.** The 3:1 rule is the LENGTH heuristic, and #63 put
      the engine's own path in front of it: a move whose module names it is posed by looking that name up
      in the model's block D, and the length match is the fallback for a move with no name. So the question
      is not whether the two Arachner moves fit the rule — it is whether anything consults the rule for
      them. Measured over four maps with creatures in them:

          JAIL2   1500 poses by name, 0 named but no position, 0 unnamed, 0 with no such name in block D
          BASE1   1158 by name, 0 named but no position, 42 unnamed
          BIGGUN   894 by name, 0 named but no position,  6 unnamed
          BOSS1    519 by name, 0 named but no position, 81 unnamed

      **The name path never misses.** Not one pose across those runs resolved a name and failed to place
      it, and not one name was absent from its model's block D. `"Sway"` (16-24) is a named move, so it is
      posed by name and the clip it "should" have by length is never asked for.

      What is left of the residue is exactly one move: 25-33, which is UNNAMED and therefore does take the
      length path — where its 9 frames want a 27-frame clip the Arachner does not have, so it falls to
      `q2_model_anim_by_length`'s ordinal tie-break and may take a neighbouring clip. One unnamed move on
      one creature, and the fact that stands behind it — two 9-frame moves and two unused 30-frame clips —
      is a statement about the disc's authoring rather than about anything the port runs.

- [x] 61. **Why the Tank Commander is still silent, given its sounds ARE on the disc (#60).** *(Answered by #98: the module registers by name, and the play site names a BSS word.)*
      First move was to read the sound addresses the decoder reports for it — `sound(80102110)`,
      `sound(80102118)`, `sound(8010211C)`. All three are **zero in the module image**: BSS, filled at
      runtime. So those are not names, and no amount of reading them will produce one.

      That is not what distinguishes the Tank Commander, though. The Berserk's `sound(8010175C)` is
      **equally zero**, and the Berserk makes sounds. Both modules store a runtime handle in BSS; the port
      resolves a creature's sounds through `q2_creature_sound_names()`, which reads a NAME TABLE out of the
      module image, the same way move names are read. So the question is not "where is the address" but
      **whether the Tank Commander's name table is being found**, and that is where this resumes.

      Two counts worth carrying into it. Scanning the disc image for VAG headers gives 63 entries named
      `tnk_*` (#60). Scanning for the bare strings gives more than that — `tnk_death` occurs 35 times of
      which 13 are VAG names, and the disc carries only 15 creature module instances in total, so the
      remaining ~19 are neither VAG headers nor module copies. Something else on the disc lists these names,
      and finding what will probably answer this question and the "which bank holds them" question together.

      **Located.** Printing every creature's table shows the finder works for five of seven and fails for
      exactly two:

          Soldier  8  sol_idle1 sol_sght1 sol_pain1 sol_pain2 sol_pain3 sol_deth1 sol_deth2 sol_deth3
          Insane   3  insane2 insane9 msc_udeath
          Arachner 6  ara_melee1 ara_idle1 ara_srch1 ara_sght1 msc_udeath Melee
          Gunner   6  gun_pain1 gun_death1 gun_sight1 gun_srch1 gun_idle1 msc_udeath
          Infantry 4  inf_pain1 inf_pain2 inf_deth1 inf_deth2
          Tankcomm 0
          Berserk  0

      `q2_creature_sound_names()` finds the module's own name string and then takes the first run of three
      consecutive 12-byte name slots after it. For the Tank Commander and the Berserk that heuristic returns
      nothing, so both are silent for the same reason — **and `main.c` carried a comment claiming "all seven
      creatures make their own sounds", which is now corrected in place.**

      So this is one bug, not two, and it is in the finder rather than in the data.

      **Fixed.** The anchored scan starts at the module's own name string; when that name is not in the
      image, `name_off` is 0 and the function gave up. It now falls back to scanning from offset 0, with the
      run-of-three test doing the validating either way. The five that already worked are byte-identical,
      because the fallback only runs when the anchor is missing:

          Tankcomm 0 -> 8   tnk_idle1 tnk_pain tnk_death tnk_step tnk_sight1 pt1__strt
                            tnk_atck1 msc_udeath
          Berserk  0 -> 13  Attack1 Attack2 Attack3 ber_pain2 inf_pain1 ber_deth2 ...

      **The Tank Commander's list is exactly the five names #60 found on the disc as VAGs, plus `tnk_idle1`
      (which has no VAG), `tnk_atck1` and `msc_udeath`.** In play, WASTE4 zone 0 goes from 4 resolved sounds
      to **6, with 0 not in bank** — the Tank Commander has a voice for the first time.

      **The Berserk's list is NOT trustworthy yet** and is flagged rather than claimed: `Attack1`, `Attack2`
      and `Attack3` are move names, and `inf_pain1` / `inf_deth2` / `inf_atck2` belong to the Infantry. The
      whole-image scan is landing on a run that begins before the real table. Its first genuine entry is
      probably `ber_idle1`. Slot INDEX is the sound number, so a table that starts early is worse than none —
      **the Berserk's numbering should not be relied on until that start is pinned.**

      **Two attempts at pinning it, both reverted, both instructive.**

      *Reject a candidate whose first slot is a MOVE name.* The move names are already readable and
      `Attack1/2/3` are three of the Berserk's own moves, so the discriminator is sound. But rejecting a
      start advances the cursor by four bytes, which lands MID-STRING: the Berserk then reported
      `ck1  ck2  ck3  pain2  pain1  deth2 ...` — the tails of `Attack1`, `ber_pain2`, `inf_pain1`. Names that
      look entirely plausible and are the ends of other names.

      *Also require a slot to START a string (`image[at-1] == 0`).* This fixes the mid-string reads and is
      principled. It also **dropped `ara_melee1` from the Arachner**, taking it from 6 names to 5 — and slot
      index IS the sound number, so every Arachner sound would have shifted by one. A refinement that fixes
      the creature you are looking at and silently renumbers one you are not.

      *Skip the MOVE-name table by EXTENT rather than rejecting one candidate.* The move-name pointers give
      the table's span, so the scan can resume past `mv_hi + 20` instead of advancing four bytes into the
      middle of a string. This is the right shape and it changed nothing — the Berserk still starts at
      `Attack1`, so the hit is not inside the span the move-name pointers describe. Reverted too.

      **Three attempts, three reverts, and the third says the model of the problem is wrong**: the run the
      scan finds is not the move-name table as read by `q2_creature_move_names`, even though its contents are
      move names. Something else in that module carries those strings.

      **A fourth idea, tested and also rejected.** Every other table's shape was censused looking for an
      anchor to end the run on: `msc_udeath` is the last entry for the Tankcomm, Insane, Gunner and Berserk,
      **absent** for the Soldier and Infantry, and **mid-table** for the Arachner (index 4 of 6, with
      `Melee` after it). Four of seven is not an anchor.

      What the census does show is that the Berserk's run is **thirteen entries where no other creature
      exceeds eight**, and that its first three are the move names `Attack1/2/3`. Dropping exactly those
      three gives ten, ending at `msc_udeath` — a plausible table. **It is not adopted**, because "plausible
      and the right length" is the same evidence that supported the ×5, the consecutive-clip runs, the
      off-by-one and the loop hypothesis, all four of which this file has had to withdraw.

      Four approaches have now failed on this, and the failures agree on one thing: the discriminator is not
      in the name run itself. It wants whatever code READS the Berserk's table at runtime, which is a
      different search from any tried here.

      **The fifth attempt did that, and the answer is that the Berserk has no name table to find.**
      Disassembling its module and looking for what WRITES the sound slots:

          801008A4  lbu  v0, 10(t0)      ; bytes from module+0x1A2
          801008B0  or   a2, a2, v0      ; ...packed into a2
          801008B8  lw   v0, 36(s2)      ; an IMPORT
          801008C0  jalr v0
          801008CC  sw   v0, 5976(v1)    ; the handle -> module+0x1758, the "sound" slot

      The slot is filled with **an import call's return value**, and that import's argument is assembled from
      packed BYTES near `module+0x1A0` — not from a 12-byte name. The same shape repeats for +0x175C,
      +0x1750 and +0x1754.

      ~~So the table is not there.~~ **Wrong — and reading the packed bytes as ASCII shows why.** The
      argument being assembled is not an index or an ID: it is **the name itself, passed by value**. The
      compiler could not use aligned loads, so it rebuilds a 12-byte string in registers a byte at a time.
      Dumping the Tank Commander's source region:

          801001E8  "tnk_" "step"          -> tnk_step
          801001F4  "tnk_" "sigh" "t1.."   -> tnk_sight1

      and the Berserk's:

          8010018C  ber_deth2
          80100198  "ber_" "idle" "1..."   -> ber_idle1
          801001A4  "ber_" "atta" "ck.."   -> ber_attack
          801001B0  "inf_" "atck"          -> inf_atck2

      **Both creatures have a real 12-byte-stride name table**, and `q2_creature_sound_names()` is looking
      for exactly the right structure. Four attempts failed at finding the Berserk's START, not because the
      table was absent, but because the heuristic anchors on the module's own name string and the Berserk's
      module does not carry one where it looks.

      **And now there are anchors.** The entries above are confirmed by the code that reads them, so the
      Berserk's table is pinned at `module+0x18C` onward. The heuristic's current output disagrees with it —
      it interleaves `inf_deth2` between `ber_deth2` and `ber_idle1`, which the module does not — so its run
      is still misaligned, but the correct addresses are known now rather than guessed.

      **A sixth attempt, using those anchors, and it made things worse.** Two rules were tried together:
      require a run's START to have no name-like slot 12 bytes before it, and reject a run whose first slot
      is one of the module's own move names, stepping past the whole run rather than four bytes.

      The first rule changed nothing — `Attack1` is itself a run start. The second took the Berserk from 13
      names to **0**, which says something the disassembly had not: **the move names and the sound names are
      one contiguous run.** `Attack1, Attack2, Attack3` are followed immediately by `ber_pain2` and the rest,
      so "skip the run" skips both tables and lands past everything.

      Reverted — 13 flagged names beat 0. But the failure is informative: the two tables abut, so no rule
      that treats a run as a unit can separate them.

      **A seventh angle: ask the disc which of those names are real sounds.** The same VAG scan that settled
      the Tank Commander (#60), run over the Berserk's thirteen:

          ber_pain2    8 VAG entries
          Attack1      0        Attack2   0        Attack3   0
          ber_idle1    0        Melee     0        ber_srch1 0

      So `ber_pain2` — index 3, the first entry after the three move names — **is a real sound on the disc**,
      and the three before it are not. That is the first evidence for where the table starts, and it points
      at exactly the candidate rejected earlier for lack of any.

      ~~It contradicts the module.~~ **There was no contradiction — the arithmetic was mine.** Dumping the
      region at a 12-byte stride settles it:

          80100144  Attack1      80100180  ber_deth2     801001BC  ber_sight
          80100150  Attack2      8010018C  inf_deth2     801001C8  ber_srch1
          8010015C  Attack3      80100198  ber_idle1     801001D4  msc_udeath
          80100168  ber_pain2    801001A4  ber_attack    801001E0  "Clear Init B..."  <- run ends
          80100174  inf_pain1    801001B0  inf_atck2

      `ber_deth2` is at `+0x180`, not `+0x18C`; `+0x18C` is `inf_deth2`. The earlier note computed
      `0x198 - 12` and assumed the answer, which put one entry in the wrong place and manufactured the
      conflict. **`inf_deth2` does sit between `ber_deth2` and `ber_idle1`, exactly as the heuristic said.**

      **And `Attack1` is a sound name.** `0x801002D4` loads `module+0x144` and packs its bytes into the
      sound import the same way every other entry is packed:

          801002D4  addiu t1, v0, 324     ; 0x80100144 = "Attack1"
          801002DC  lbu   v1, 1(t1)
          801002E0  lbu   t0, 324(v0)

      So the table runs from `+0x144` to `+0x1D4`, thirteen entries, and **the heuristic's output is
      correct**. The Berserk's names share three strings with its move names because the creature's attacks
      are called the same thing, not because the scan drifted.

      Seven attempts, and the answer is that nothing was wrong. The flag on this table is removed.

      One more thing the scan settles: `ber_idle1` has no VAG either, yet the disassembly proves it IS a
      name the module passes to the sound import. So **a name being requested does not mean a VAG of that
      name exists** — `tnk_idle1` was the same (#60). Both are *idle* sounds, which suggests the disc simply
      ships without them, and it means the VAG check is evidence FOR a name but not against one.

      **The Tank Commander is the same shape**, checked rather than assumed:

          80100638  addiu a2, a0, 500     ; module+0x1F4
          8010063C  lbu   v1, 1(a2)       ; packed bytes again
          8010062C  jalr  v0              ; an import
          80100648  sw    v0, 8464(s6)    ; the handle -> module+0x2110

      So neither creature fills its sound slots from a name table; both call an import with packed byte
      arguments. **The name run is not the mechanism for either of them.**

      What separates the two is corroboration, not structure. The Tank Commander's eight names were checked
      against the disc independently — five of them are VAG entries with those exact names (#60) — so its
      run is real data even though it is not what fills the slots. The Berserk's thirteen have no such
      check, and three of them are its own move names, so its run is a false positive.

      That distinction is worth keeping precisely because it is not visible in the code: two creatures with
      identical slot-filling, one whose name run happens to be its sound names and one whose does not. The
      only thing that told them apart was going to the disc and looking for VAGs.

      The state that ships is the plain fallback: **the Tank Commander correct and verified
      against #60's VAG names, the Berserk found but flagged.** A wrong table is worse than a missing one
      here, and the Berserk's is wrong in a way that reads as right — which is the whole reason it is
      labelled rather than used.

      ---

      **The Arachner's two moves: five hypotheses tested, five rejected.** Listed so none is tried a sixth
      time:

        1. *They span consecutive clips.* Every one of the five had such a run — but **89% of arbitrary
           lengths** do on that chain. A test almost everything passes distinguishes nothing.
        2. *An off-by-one in the range.* The move record, the frame array and the name table **all three**
           say nine.
        3. *A variant model.* The Arachner's is the only one on the disc.
        4. *Looping moves carry an extra frame to close the cycle.* **25 of 27** looping moves match exactly.
        5. *Locomotion is special* — the two are `via 3` and `via 4`, walk and run. But **15 of 17** walk/run
           moves across the disc match exactly:

               walk/run   17 moves   n*3 15   (n+1)*3 2   neither 0
               other      98 moves   n*3 97   (n+1)*3 1   neither 0

      Each hypothesis was built on the residue and each fitted it perfectly, which is exactly why none
      survived a null test.

        6. *The frame records differ.* Dumping them (POWER1 carries the Arachner module) rather than
           reasoning about them — the move the Berserk taught:

               16-24  (miss)     02 0A 00 | 02 0A 00 | 02 0A 00 ...   nine frames
               25-33  (miss)     04 00 00 | 04 00 00 | 04 00 03 ...   nine frames
               130-132 (matches) 04 00 00 | 04 00 00 | 04 00 00
               40-45   (matches) 05 00 00 | 05 00 00 | 05 00 00 ...

           **Rejected as the discriminator**: `25-33` has the same middle byte as the moves that match. But
           it is not nothing — `16-24`'s middle byte is `0x0A` where every other Arachner move examined has
           `0`, and 16-24 is its walk. So the frame record's middle byte is a **per-frame distance**, set
           only on locomotion, which is a field this project had not identified. It explains what the byte
           is without explaining the clip mismatch.

        7. *The module and the model come from different maps.* POWER1 carries the Arachner's module and
           JAIL4 its model, so the clip list being compared might not be the one that module drives.
           Checked: **both maps ship the same thirteen clip lengths.** The assumption held.

      **What the seventh attempt did establish** is that the two `16-24` records are one animation, not two.
      Their frame arrays differ in exactly one byte per frame:

           walk (via 3)   02 0A 00 | 02 0A 00 | ...
           run  (via 4)   03 0A 00 | 03 0A 00 | ...

      Same range, same per-frame distance (10), same think — only the AI verb differs. So the Arachner has
      **thirteen distinct animations for thirteen clips**, eleven of which match by the 3:1 rule, leaving
      exactly two moves (`16-24` and `25-33`, nine AI frames each) and exactly two clips (30 frames each)
      unpaired.

      That is as tight as the residue gets: two and two, wanting 27 where 30 is on offer. Three ticks short,
      twice, with the frame count confirmed four independent ways. The honest state after seven hypotheses is
      that **no property yet examined separates them from the 113 that fit**, and the arithmetic of the gap is
      now exact rather than approximate.

      **Do not conclude the sounds are absent.** That was the previous answer here for many passes and it was
      wrong; see #60 for how the mistake was made and how it was caught.

- [x] 62. **The 3:1 length rule and the disc's own NAMES disagree about which clip a move plays.**
      An eighth angle on the Arachner: match its AI moves to block-D moves **by name** instead of by length.
      Both sets of names come off the disc — the AI's from the module's move-name table, the model's from
      block D — so if a creature's move is called `Melee`, the animation it should play is surely the one
      the model calls `Melee`.

          name          AI range   n*3  blkD  verdict
          Start Melee   0-12        39     9  conflict
          Sway          16-24       27    39  conflict
          Attack 1      94-109      48    30  conflict
          Rear          130-132      9    48  conflict
          Start Attack  133-135      9     9  AGREE
          Pain 1        40-45       18    15  conflict
          Melee         35-39       15    36  conflict
          Attack 3      110-129     60    48  conflict
          Death 2       53-64       36    60  conflict
          Walk          136-138      9    30  conflict
          Pain 2        78-93       48    18  conflict
          Stand         65-77       39    39  AGREE

      **Two of twelve.** Under the length rule the Arachner's `Melee` plays the clip block D calls `Pain 1`,
      its `Walk` plays `Start Attack`, and its `Rear` plays `Start Melee`. Those are not near-misses; they
      are different animations.

      **This does not simply overturn #51h**, and the tension is the point:

        * The length rule's null test is real — 96 of 101 across seven creatures against a 33% chance
          baseline. AI lengths times three land on clip lengths far more often than chance allows.
        * The name disagreement is also real, and names are the disc's own labels rather than an inference.

      Both cannot be the assignment mechanism. Either the AI move names are paired to ranges wrongly by
      `q2_creature_move_names` (which would make this table meaningless), or the length agreement is a
      systematic coincidence and the port has been choosing wrong clips at a 95% "hit rate" all along.

      **Now checked on all seven, and the conflict is universal:**

          Soldier    2 of 29 named moves agree with block D by name
          Tankcomm   1 of 16          Gunner     0 of 13
          Insane     1 of 18          Infantry   1 of 11
          Arachner   2 of 12          Berserk    0 of 10
          -----------------------------------------------------
          TOTAL      5 of 97

      So it is not an Arachner quirk. For **every** creature on the disc, the clip picked by matching AI
      length x 3 is a different clip from the one that shares the move's name, roughly 95% of the time.

      Both name sources are solid, which is what makes this hard to dismiss. The AI side comes from
      `q2_creature_move_names`, which matches a 20-byte record's two u16 frame bounds **exactly** against a
      decoded move — not a fuzzy match. The model side is block D read straight out of the file.

      And name-matching does not simply replace the rule, because the ratios it implies are inconsistent:
      the Soldier's `Death1` is 35 AI frames against a 108-frame clip (3.09), its `Pain1` 7 against 15
      (2.14). A mechanism should not give 3.09 and 2.14.

      **Resolved in favour of the length rule, by totals.** If an AI move's frame range and a block-D span
      really measure the same thing at 3:1, then summing each creature's DISTINCT move lengths and
      multiplying by three must equal the sum of its clip lengths. It does:

          creature    AI*3  model    diff  diff/3
          Insane       756    756       0     0.0
          Gunner       570    570       0     0.0
          Infantry     579    579       0     0.0
          Berserk      363    363       0     0.0
          Arachner     384    390       6     2.0
          Soldier     1275   1302      27     9.0
          Tankcomm     780    762     -18    -6.0

      **Four of seven balance to zero.** Not approximately — exactly, across hundreds of frames, on a rule
      that would drift visibly if the ratio were anything other than three. The premise the previous entry
      doubted is sound: the two timelines are the same timeline, at exactly 3:1.

      So the name disagreement is the thing that is wrong, not the ratio. Either `q2_creature_move_names`
      pairs AI ranges to module names incorrectly, or a module's move names and a model's block-D names are
      simply different vocabularies — the same creature calling its animations one thing in its AI and
      another in its mesh. The second costs nothing to believe and explains everything.

      **And the Arachner's residue is now exactly 2 AI frames**, arrived at independently: its model carries
      6 model-frames more than 3x its AI total, which is two moves one frame short — the same conclusion the
      per-move matching reached, from totals that never looked at an individual move.

      **The Tankcomm's -18 is fixed, and the totals proved it.** Its AI used 6 frames more than its model
      carries, and searching every creature for overlapping ranges found exactly one on the whole disc:
      `65-70` lies inside `55-70`. A move cannot span frames another move owns, so `55-70` is a merged span
      the name-record splitter could not see — no name marks that boundary, but another decoded move does.

      `split_overlapping_moves()` shrinks a move that strictly contains another, and the arithmetic closes:

          creature    AI*3  model    diff
          Tankcomm     762    762       0     <- was -18
          Insane       756    756       0
          Gunner       570    570       0
          Infantry     579    579       0
          Berserk      363    363       0
          Arachner     384    390       6
          Soldier     1275   1302      27

      **Five of seven exact.** This is the second merge bug found in the move decoder — the Soldier's came
      from name records, the Tank Commander's from an overlap — and both were invisible until a total was
      compared against something that had to equal it.

      **The Soldier's +27 and the Arachner's +6 are the same phenomenon, and it is not a mismatch.** Mapping
      the Arachner's frame numbering:

          covered : 128 of 139 frames, 0..138
          gaps    : 13-15 (3)   34 (1)   46-52 (7)   — 11 frames belonging to no move

      128 x 3 = 384 against 390 model frames. The arithmetic tempts one last story: extend `16-24` to
      `15-24` and `25-33` to `25-34`, both using an adjacent gap frame, and 130 x 3 = 390 exactly. **Refuted
      the same way as before** — the frame arrays hold nine entries. `16-24`'s runs `0x801017C0..0x801017DA`,
      twenty-seven bytes and a pad, ending where its move record begins. There is no tenth frame.

      So the residue is not two moves that fail a rule. It is **six model frames the AI never reaches**,
      which is exactly what the Soldier's +27 is — nine AI-frames of animation with no move pointing at it.
      Five creatures use every frame their model carries; two do not. Spare animation on a shipped disc is
      ordinary, and #51h's five "exceptions" were the same fact seen from the per-move side.

      ~~What remains open is which clip those two Arachner moves DO play.~~ **The question is malformed, and
      `model.h:436` said so before this session began:**

          "So a model's clips are ONE CONTINUOUS TIMELINE and the animation position is an offset into it;
           a clip boundary is wherever the subtractions happen to fall. That matters to anything driving a
           model from a creature, because it means THERE IS NO CLIP INDEX TO FIND: a move's frames map onto
           this timeline and the walk lands in the right clip on its own."

      The engine never selects a clip. `0x8007E9DC` computes a position from the frame and `0x8006B924`
      walks the chain subtracting durations until it lands. A move is under no obligation to begin or end on
      a clip boundary, and one that spans two is not an error.

      So the whole frame collapses:

        * The 96/101 statistic is not a lookup rule succeeding. It says most moves **happen** to align to
          clip boundaries, which is a fact about how the animations were authored.
        * The name-versus-length conflict dissolves — neither is a lookup, because there is no lookup.
        * The Arachner's two moves land mid-clip or across a boundary. That is ordinary.
        * `q2_model_anim_by_length()` is a port-side substitute for a mechanism that selects nothing, which
          is what #51c already recorded: *the port's constant is not wrong; the port is missing a step.*

      Nine hypotheses were spent on a residue that a paragraph in `model.h` had already declared a
      non-question. The lesson is the one this session keeps paying for: **read what the repo already knows
      before deciding what is unknown.**

      ~~So both readings are in trouble, and that was the honest state.~~ The length rule has a real statistic
      (96/101 against a 33% baseline) and assigns names that disagree; name-matching has the disc's own
      labels and implies no consistent ratio. One of the two premises underneath them — that an AI move's
      frame range and a block-D move's span measure the same thing — is probably false, and that is where a
      next pass should dig rather than at either rule.

      Worth noting the port is not visibly broken by this: captures across many maps render creatures
      animating plausibly. That is weak evidence — a wrong-but-same-length clip looks like an animation —
      but it does mean this is a fidelity question rather than a crash-or-garbage one.

- [x] 63. **What implementing the real animation path still needs, stated exactly.** *(Closed by #97: every named move now poses by name.)*
      Everything about the engine's mechanism is now read, so it is worth writing down what is and is not in
      hand — the answer is one missing link, not a system.

      **What the engine does** (#51b, #51c, #51d, #47):

          position = base + 30 * (ai_frame - move.first)          0x8007EA44
          then walk the clip chain subtracting durations          0x8006B924
          position is in TENTHS of an animation frame             0x8006B5D8 divides by 10
          30 per AI frame / 10 per model frame = 3 model frames per AI frame

      No clip is selected; the walk lands wherever the subtractions fall (`model.h:436`).

      **What the port has**: `q2_model_move_get/count`, the block-D layout `{name[12], start, end, rest,
      one}` with `start`/`end` in 2-per-frame units, `move i drives clip i` verified 34/34, the AI move
      ranges out of each creature module, and `q2_model_anim_at()` which already walks a tick to a clip.

      **The one missing link**: the engine's runtime record carries `first`, `last` AND `base` together —
      AI frames and position in the same record — so the mapping is internal to it. Block D carries
      `start`/`end` (positions) and a name, but no AI frames. The creature module carries AI frames but no
      positions. **Nothing on the disc has been found that pairs a given AI move to a given block-D move**,
      and that pairing is precisely what the runtime table's load-time construction supplies (#51f, still
      unread).

      Matching by length is what this port does and it lands 96 of 101, but #62 showed the names disagree on
      92 of 97, so length matching is a coincidence-driven substitute rather than the pairing.

      So: `position = block_d[k].start * 5 + 30 * (ai_frame - ai_move.first)` is the formula, `* 5` because
      block D counts 2 per frame and the position counts 10. Every term is known except **which k goes with
      which AI move**.

      **ANSWERED: the pairing is a lookup BY NAME, and `0x8006D330` is it.**

          8006D334  sw   a1, 20(sp)      ; the wanted name, three words passed BY VALUE
          8006D338  sw   a2, 24(sp)
          8006D33C  sw   a3, 28(sp)
          8006D340  lw   a1, 56(a0)      ; a1 = model->ofs_block_d
          8006D34C  lw   v1, 0(a1)       ; the record's word 0 — its NAME
          8006D364  lw   v0, 20(sp)      ; the wanted name's word 0
          8006D36C  bne  v0, v1, next    ; compare, twelve bytes as three words
          8006D374  lw   v0, 24(sp)      ; ...word 1
          8006D378  lw   v1, 4(a1)

      So the engine does not index block D and does not match by length. It **walks block D comparing the
      12-byte name field** against a name handed in by the caller, three words at a time, and returns the
      matching record. The same by-value name passing the creature sound imports use (#61) — this codebase
      does that wherever a 12-byte string crosses a call boundary.

      That makes `q2_model_move_by_name()`, added earlier in this session, exactly the right API and
      `q2_model_anim_by_length()` exactly the wrong one. It also explains why #62's name comparison found
      92 conflicts of 97: names ARE the mechanism, so the conflict means the port is pairing AI ranges to the
      wrong names, not that names are irrelevant.

      **And the name the caller passes is a string literal in the EXE.** `0x8003C854` and `0x8003C8EC`
      materialise `0x800AC584` and `0x800AC590`, and that region is a 12-byte-stride table of its own:

          800AC560  Run          800AC59C  Jump
          800AC56C  Attak        800AC5A8  Pain 1
          800AC578  Death 1      800AC5B4  Pain 2
          800AC584  Death 2
          800AC590  Death 3

      `Run`, `Attak`, `Death 1/2/3`, `Jump`, `Pain 1/2` — **a player's animation set**, not a creature's, and
      `Attak` is spelt that way on the disc. So `0x8003CBxx` is the player-model path, and it reaches its
      animations exactly as predicted: build a 12-byte name, hand it to `0x8006D330`, get a block-D record.

      So the chain is complete for the player and the mechanism is confirmed twice over. For a creature the
      same lookup must be handed the creature's own move name, which is the table
      `q2_creature_move_names()` already reads — the names on both sides ARE the same strings
      (`Sway`, `Walk`, `Melee`, `Death 2` appear in the Arachner's module and its block D alike).

      **And that last question answers itself from the record layout.** The runtime record the engine walks
      holds `first`, `last` and `base`; block D holds `start`, `end` and a name. Put the two together and
      what each field is becomes plain:

          first, last   AI frames — how long the creature SPENDS in the move
          base          where in the model's timeline that move begins
                        (block D's `start`, x5, since block D counts 2/frame and position counts 10)
          position      base + 30 * (ai_frame - first)

      **These are different quantities and nothing requires them to agree.** An AI move's length is a
      duration; a block-D move's span is an animation's extent. A move whose duration is shorter than its
      animation simply stops partway; one that is longer runs on into whatever follows on the continuous
      timeline — which `model.h:436` already said is legal, because a clip boundary is only where the
      subtractions fall.

      So #62's 92-of-97 is not a defect and never was. It is the observation that a creature's AI durations
      and its animations' extents were authored independently, which is what one would expect. The 3:1 that
      #51h measured is not a matching rule at all — it is just `30 / 10`, the ratio between the two units,
      and the 96/101 is how often the two happen to coincide.

      **The chain is closed**: name -> block D record -> `base` -> `position = base + 30*(f - first)` ->
      walk the clip chain. Every term read from the disc, nothing inferred.

      **And it is now code.** `q2_model_position_for_move()` looks the move up by name — the engine's own
      mechanism, `0x8006D330` — and returns

          position = block_d[name].start * 5 + 30 * (ai_frame - move_first)

      which feeds `q2_model_anim_at()`, the walk this port already had. Together they replace
      `q2_model_anim_by_length()`, which matches a clip by length: something the disc never does.

      `tests/model` pins the arithmetic against the Arachner's real table — its `Walk` starts at block-D 360,
      so position 1800, and each AI frame adds 30 — and asserts all three units (2 per frame in block D, 10
      in the position, 30 per AI frame). Getting any of them wrong is a silent mis-pose rather than a
      failure, which is why they are pinned as constants rather than left implicit in one formula.

      **Wired into the draw path**, with the move names plumbed through `q2_creature_bind_move_names()` —
      the bind does not own the module image, so the names are read once in `creworld.c` and bound the same
      way the thinks are. The client tries the name path first and falls back to length matching when a
      module names a move its model's block D does not carry.

          BASE2   1745 poses by name, 2655 by the fallback
          BASE1   1386 / 1814
          JAIL4   1775 / 1425

      **Two bugs the counters caught that a screenshot could not.** The first: the guard
      `if (ai_frame < move_first) return false` rejected every pose, because a monster whose frame counter
      has not caught up sits at frame 0 while its move starts at 146 — the surrounding code had always
      clamped that, and the fix is to use its clamped value. The second, and the reason the first fix
      changed nothing: **`q2_model_anim_at()` walks in whole model frames while the position is in tenths**,
      so passing the position straight in overran the timeline and failed on every creature, silently, back
      to the length path. `pos / 10` is the conversion.

      Both showed as `0 by name` and a pixel-identical frame. Nothing looked wrong at any point, which is
      the whole argument for the counter: 110 of 128 module move names ARE in their model's block D, so a
      path that never once succeeded should have been suspicious, and only a number said so.

- [x] 64. **The first code found that walks block D — and it names the `rest` field. CLOSED: the pairing it pointed at was found from the other side.**
      Hunting the load-time transform (#51f, #63) by asking who reads `model+0x38`:

          8006D310  lw    v1, 56(a0)     ; v1 = model->ofs_block_d
          8006D318  lw    v0, 0(v1)      ; the record's first word
          8006D320  bne   v0, zero, ...  ; a ZERO first word ends the run
          8006D324  addiu v1, v1, 20     ; 20-byte stride

      That is block D, walked exactly as `model.h` describes it, and it is the **only** function on the disc
      that does so. It has one caller, `0x8003CC00`, and the two instructions either side of that call are
      more informative than the walk:

          8003CBEC  lhu  v1, 12(a1)      ; a block-D record's `start`
          8003CBF4  sh   v1, 16(a1)      ; ...written to its `rest`
          8003CBF8  lhu  v1, 12(a2)      ; and the same for a second record
          8003CC04  sh   v1, 16(a2)

      **So `rest` is a CURSOR, and this resets it to `start`.** This project had only been able to say that
      `rest` equals `start` on 15 records and `end` on 77 — a statistic with no meaning attached. It is the
      record's current position, and a record sitting at its `end` is one whose animation has finished.
      That also explains the split: the 15 at `start` are rewound, the 77 at `end` have played out, and the
      disc simply ships whatever state the authoring tool left.

      **This is where #63's missing pairing should be looked for.** The function around `0x8003CBE8` holds
      live block-D record pointers in `a1` and `a2` and reaches for two globals (`gp+17252`, `gp+17280`);
      whatever supplies those pointers knows which record belongs to what, which is precisely the link
      `position = block_d[k].start * 5 + 30 * (ai_frame - first)` is missing.

      **It was not needed.** #63 found the pairing from the other end — a move is matched to its block-D
      record by NAME, through the 12-byte compare at `0x8006D330` — and that path is now measured as
      universal: across four maps with creatures in them, 1500 / 1158 / 894 / 519 poses resolve by name and
      **not one name is absent from its model's block D** (#59). There is no missing pairing left to find
      through the pointers. What this entry contributed and keeps is the reading of `rest`: it is a
      CURSOR, and `0x8003CBE8` rewinds it to `start`.

---

## Models had no backface rejection, and the world's rule is not the model's

Chasing #10b's violet head meant reading the model draw end to end, and the thing that came out of it is
not what the chase was for: **`q2_model_build_ot` emitted every face of every model, both sides of it.**

The world has had rejection since the sealing pass — `q2_world_quad_faces_camera`, an NCLIP pair
transcribed from all three of the world's quad linkers. Nothing equivalent existed on the model path, and
because the ordering table has no depth buffer, "emitted" means "will paint over whatever the sort put
behind it". On a closed mesh that is the far side of the model showing through the near side.

**The linker is at `0x800B2410`** — the same function whose NCT writes the per-vertex colours this port
already transcribes — and it loads the corners exactly as the world's does:

    800B2478  srl  v1, t6, 21     ; file v3 ...
    800B248C  mtc2 v0, SXY2       ; ...into SXY2
    800B2498  bltz t3, 0x800B24F0 ; force-draw bit: skip the test entirely
    800B24A0  nclip               ; on (v0, v1, v3)
    800B24C0  mtc2 v0, SXY0       ; SXY0 = file v2
    800B24C8  blez t8, 0x800B250C ; MAC0 <= 0 -> draw
    800B24D0  nclip               ; on (v2, v1, v3)
    800B24E0  bgtz t8, 0x800B250C ; MAC0 >  0 -> draw
    800B24E8  b    0x800B2528     ;             otherwise store NULL: dropped

**The comparisons are the world's exchanged.** The world's are `bgtz` then `bgez`-to-drop
(`0x800AF8B4` / `0x800AF8D4`); the model's are `blez` then `bgtz`. Same registers, same corner order,
opposite sign — so **model quads are wound the other way round from world quads**. That is a fact about
the two authoring pipelines, and it is the sort of thing that is invisible until something asks.

**Measured rather than eyeballed, because "looks better" nearly sent this the wrong way.** A first pass
scored the two candidate signs by looking at the render and picked the *inverted* one, which drops the
near faces and shows the model's inside — and which looked cleaner only because it happened to hide the
violet. Averaging each group's projected depth settles it without appeal to taste: over the Soldier the
rule above keeps faces of mean depth **1174** and drops mean **1233**, and the same split holds on **all
fourteen** of its parts. The kept set is the nearer one, which is what front-facing means.

Face counts, at the same camera:

| | before | after |
| --- | --- | --- |
| `Soldier` | 233 | 129 |
| `Infantry` | 230 | 119 |
| `Chest` | 52 | 15 |
| BASE1, 22 creatures in frame | 5,120 | 3,589 |

Pinned in `tests/test_world.c` beside the world's, and deliberately beside it: the test asserts that the
same square one rule draws is one the other drops, in both directions, so a later "simplification" that
folds them into one helper with a sign flag fails rather than silently halves the game's models.

- [x] 65. **RESOLVED: the mask is block A's third field, it is zero on every model on the disc, and the
      reason it looked unreachable is that `0x800B2410` is not a function.**

      **The mistake that cost this entry three passes, mine included.** `0x800B2410` has no `jal`, no
      materialised address and no pointer word anywhere in the image, and I recorded that twice as
      "the linker has no caller". It is not an entry point. Scanning back to the previous `jr ra` puts it
      inside the function that begins at **`0x800B1F90`** — which is called, once, from `0x8006DCBC`. Both
      of the NCLIPs at `0x800B24A0` and `0x800B24D0` belong to that function. Searching for callers of a
      LABEL will always find none, and the negative result reads exactly like a discovery.

      **What `a0` holds there.** `0x800B22FC` loads `a0` from the descriptor's `+0x14`, and the code then
      walks it in eight-byte steps testing `lh 0(a0)` for zero. That array is the model's **block A** — the
      directory this project already documents as `8 x {u16 count; u16 offset; u32 0}`. So:

          +0x00  u16  count   how many faces this batch holds
          +0x02  u16  offset
          +0x04  u32  force   ONE BIT PER FACE, most significant first

      `lw t3, 4(a0)` is that third field; `sll t3, t3, 1` per face walks it from the top; `bltz t3` tests
      the sign and skips the NCLIP test outright. The field an earlier pass recorded as the constant `0`
      IS the force-draw mask.

      **And it is zero everywhere: 0 of 13,784 block-A entries across all 1,723 models on the disc.** So
      the escape never fires, this port's unconditional culling was right all along, and the worry this
      entry ended on — "a two-sided surface on some model somewhere is drawn on one side only until that is
      read" — is refuted. There is no such surface on this disc.

      **Honoured anyway**, in `q2_model_batch_forces_draw`, for the same reason the loader now tolerates
      chunk names no file emits (#33): the engine supports it and a build that uses it should not be
      silently mis-drawn. Face counts are unchanged, which is the check — a mask of zero must change
      nothing.

      *A second correction, to my own working:* the descriptor field I found at `0x8006BC0C`
      (`descriptor+0x04 = arg1 + 10`) is the OUTER descriptor's, not block A's, and it is not this mask.
      The alarm I raised from it — that a pointer read as a mask would be negative and force every face —
      does not arise, because `a0` at `0x800B2410` is not the descriptor.

## The status bar's fifth byte is a palette, and the HUD had been drawing the armour box for health

Two separate colour defects were reported together — "everything is inverted, red shows as blue" — and they
turned out to share nothing but a symptom. Recording both, because the first one is a trap this project's own
tooling is structurally blind to.

**The window, not the renderer.** The client uploaded the front buffer to an `SDL_PIXELFORMAT_XRGB1555`
texture by memcpy. The console's halfword is `STP | B | G | R` with red in bits 0–4, so that enum reads B
where R is and exchanges the two channels in every pixel. Fixed to `XBGR1555`; the full argument, and why
PAL has nothing to do with it, is now FIDELITY.md §4a. What matters here is the epistemics: `--shot` captures
come off the front buffer *before* SDL sees it, so they were correct the entire time. Every comparison this
project makes is made in those captures. **A rendering claim verified only by `--shot` says nothing about
what the window shows**, and that gap had been open since the client got a window.

**The rect table is indexed, not scanned — icontable.h's "SOLVED" was wrong.** That header claimed a rect
record's fifth byte is the item's `effect` dispatch index, on the strength of the weapon-to-ammo table
reading correctly as effect ids six ways over. All three status-bar sub-draws say otherwise, in the plainest
possible terms: `lbu v0, 170(t0)` at `0x80035190` and `lbu v0, 150(a0)` at `0x8003565C` with `t0`/`a0` =
`0x8009C478`, and `a0 * 5 + t0` at `0x80035374`. Five-byte stride on five-byte records, two hard-coded
multiples of five, and no compare against the fifth byte anywhere.

The fifth byte is a **palette index**. The sub-draws store it into byte +8 of a ten-byte field record
(`0x80035210`), which is the same slot a counter running low takes 7 in (`0x8003524C`) — an effect id could
not be written there. `0x800337EC` initialises all thirteen fields with +8 = 8 and +9 = 14, and 14 is
`qk_menu.lbm`'s VRAM slot, so +9 is the sheet. Reading the three palettes the bar selects between closes it:

| index | ramp | what it colours |
|---|---|---|
| 8 | pale cyan to (160,200,224) | the numerals |
| 38 | blue to near-white | the health cross |
| 7 | red/orange to (248,64,0) | the low-value flash |

**What the wrong reading cost.** Health asked for effect 34 and the scan returned rect **30**, whose fifth
byte is 34 — and rect 30 is the *power shield*. So the health field drew a red-lamp device, and because the
wrong answer was still a real icon it read as a mis-picked sprite rather than as a mis-read table. Health is
rect **34**, which is the cross. Separately, every sprite in the bar was drawn with the menu font's single
CLUT, because the port had nowhere to put a per-sprite palette.

**And rect 30 was then written down as the armour icon, which cost a second bug.** The correction above
recorded `8003565C lbu v0, 150(a0)` as "armour, ALWAYS 150" beside health's genuinely unconditional
`lbu v0, 170(t0)`. The instruction is real; "ALWAYS" was never tested. It is one of **five arms** of a select
on the inventory flag word at `0x80035554` — `andi 0x8000` power shield → 150, `andi 0x4000` body → 130,
`andi 0x2000` combat → 135, `andi 0x1000` jacket → 140, fall through → rect 0. Rendered with their own
palettes, 26/27/28 are three armour vests in red, gold and grey and 30 is not a vest at all. So every player
wearing any armour was shown a power shield. The same shape as the mistake it was correcting, one field over:
an instruction read correctly, and a premise about its **reachability** never checked. The armour field also
has a state machine in front of it — a cells gate, a one-second alternation on the level clock, and the cells
count rather than the armour points in the power state — which had no counterpart in the port at all.

Why the join looked so convincing is worth keeping: palette indices and effect ids both run near-monotonically
over the grid and stay a small constant apart, so any window of them agrees. The two checks that break the tie
appeal to neither reading — under the effect reading the cell assigned to the power shield is **empty**, and
under the index reading the six cells the ammo table selects are six ammo boxes.

**Still open — what suppresses the ammo counter.** Retail capture at a level start shows health and its cross
and nothing else; the port now matches on armour, because `0x80035594` branches to `0x80035630` on a zero
armour value and the test at `0x80035634` skips the whole sub-draw. The ammo sub-draw has **no equivalent
early-out**. Its only zero test (`0x800353B0`) collapses the icon to the 1x1 blank and leaves the digits
alone, and the splitter at `0x80034E6C` blanks leading zeroes but always emits the units digit — so by the
code as read, a blaster-only player should see a "0" that retail does not show. Either a guard exists
somewhere not yet walked, or one of the three field pointers the sub-draw is handed is not what it looks
like. Left alone rather than papered over with a threshold: the port draws the "0" and is wrong in a way
that is visible and documented, which is better than being right by accident.

Related: the health flash holds solid at or below zero (`blez` at `0x80035248`) while the ammo flash does not
and is `sltiu`, so the two thresholds and the two zero behaviours are genuinely different and are kept apart.
Not modelled: the health sub-draw writes the flash palette to all three digit fields (`0x80035268`–`0x80035270`)
where the ammo one writes to a single field (`0x80035464`).


---

## A creature's claws had no owner, and landed on the player's own feet

`q2_combat_melee` — the transcription of `0x800612F0`, a creature's contact hit — had **no caller
anywhere in the port**. The client raised its own melee instead, and got two things wrong that no
screenshot could show.

**The damage point was the player's own position.** `0x800612F0` passes the ATTACKER's origin, and
everything downstream of that point is directional: the knockback, where the blood sprays, and the
flinch's roll, which `q2_sim_hurt_player` computes as the hit point against the view's own right vector.
Handing it the player's position makes that difference the zero vector, so `side` came out 0 on every
claw that ever landed in the game — a hit from the left rolled the view exactly as far as a hit from the
right, which is to say not at all — and the blood sprayed from the player's own feet.

**The attacker was NULL.** `q2_combat_damage` records `target->last_attacker = attacker->owner`, so a
player clawed to death died with no killer. That is the one field the attribution rule exists to carry.

Fixed on both sides: `q2_sim_hurt_player` routes a melee with an attacker through `q2_combat_melee` and
**overrides** the point rather than trusting the caller — a melee is the one mod whose point is not free
— and the client resolves which creature is swinging from the monster/actor arrays' pointer difference.

Pinned in `tests/test_sim.c`, and checked against the DEFECT as well as the fix: with the override
disabled the test reports `roll from the left -80, from the right -80` and fails; with it, `-80` and
`+80`.

Measured in the client rather than asserted. Sweeping twelve maps with `--watch`, melee reaches the
player on two of them — **JAIL2 twice, POWER2 ten times** — and on POWER2 a Berserk now claws the player
from 100 to **-3** and the death screen opens on `RESTART LEVEL`. Module action, AI tick, damage,
attribution, death: the whole chain, in one capture.

- [x] 66. **The debris burst has a caller waiting for it, and the operand it needs RESOLVES.**
      `q2_sim_debris_burst` is the other function in the sweep that is a feature rather than an accessor,
      and `sim.h` says why it has no caller: *"the port exposes it rather than wiring it into the GLASS
      primitive, because which Scene node a given pane owns is part of the UserFuncs runtime-object
      mapping and not of the effect system."*

      That mapping is no longer missing. It is the same two-buffer rebase #56 found for the rotators: an
      operand the game has already run reads -1 in COMMON's Events copy and lives at the same offset in
      the zone's. Applying it to the breakables — `GLASS` and `SHOOTTHEN`, whose object slot is a single
      `s16` at `+4` rather than the rotators' four at `+12` — and counting disc-wide through
      `q2psx-inspect zonescript`:

          breakable CALLs (GLASS, SHOOTTHEN) : 10
            too short   : 0
            no object   : 0
            usable      : 10  (4 rescued from a ZONE's copy)

      **10 of 10**, and four of them only because of the rebase. So the stated blocker is gone: a pane's
      Scene node is reachable, and with it the node's box, which is exactly what `q2_sim_debris_burst`
      takes.

      **GLASS is read and wired**, and reading `0x8002A350` settles both what a pane throws and what a
      script can make it do:

          obj = objectArray + 92 * item[+4]          ; 92-byte runtime objects
          if (item[+4] >= 0)
              debris(obj->node, obj->param_a, obj)   ; 0x8002A384, on EVERY call
          if (damage != 0) {
              item[+6] -= damage                     ; hit points, mutated in place
              if (item[+6] > 0) return                ; still standing
          }
          debris(obj->node, obj->param_b, 0)         ; 0x8002A3DC, the shatter
          sound(id, centre of the node's box)        ; 0x8002A4A4

      `0x80064558` is `q2_fx_debris_burst`, which this port already had: it takes the SCENE NODE INDEX,
      a count, and a third argument that is a fixed origin or zero — zero scatters the pieces uniformly
      through the node's box (`beq s2, zero` at `0x80064614`), which is what makes a shattering window
      come apart across its whole surface rather than out of its middle. So the hit burst comes out of a
      point and the shatter comes out of the pane. `q2_sim_breakable_call` is that, and the client's CALL
      dispatcher runs it beside the rotators'.

      **And it fires zero times, which is the useful half of the measurement.** Sweeping every trigger
      volume on the disc:

          breakable CALLs (GLASS, SHOOTTHEN) : 10
            usable      : 10  (4 rescued from a ZONE's copy)
            the trigger sweep RUNS 0, of which resolve a Scene node : 0

      No trigger volume on this disc calls GLASS. The ten CALL items exist so the LOAD-TIME constructor
      can allocate each pane's runtime object; the engine then reaches the primitive from its damage
      path, not from the script. So the scripted break is a real engine behaviour with no user on this
      disc, and the code is there because it is what the damage dispatch will call — stated rather than
      left to look like a working feature.

- [x] 67. **What is still owed: the route from a shot to a UserFuncs object.**
      `obj+0x24` is the on-damage callback the load-time constructor installs, and nothing found so far
      calls it from the weapon code. The 48-entry object array at `0x800D6BB0` has 48 materialised
      references and every one of them is inside the `0x8002xxxx` UserFuncs runtime or the two at
      `0x800688AC` / `0x80079958`, which are its reset — so the damage path does **not** scan the object
      array looking for a victim. It must arrive with the object already in hand, which points at the
      COLLISION node: the mover's two hull paths already carry a node slot per entity (`+0xA0` for
      PrimaryColl, `+0xA2` for SecondaryCol, effect.h), and a hitscan traced against PrimaryColl ends on
      a node. Whatever maps that node to a runtime object is the missing link, and it is the same shape
      as the `SpaceLights` partition, which also turned out to hang off a collision node rather than a
      scene one (#9).

## The blaster blanks its own ammo digits, and the camera had dropped a constant

Two follow-ons from the colour round, both closed.

**What suppresses the ammo counter — ANSWERED.** The previous entry left this open on the grounds that the ammo
sub-draw has no early-out and the splitter at `0x80034E6C` always emits a units digit, so a blaster-only player
should show a "0" retail does not. The branch is at the END of the sub-draw, after the digits have already been
computed normally:

```
80035498  addiu v0, zero, 1
8003549C  bne   s0, v0, 0x80035538      ; weapon != 1, leave the digits alone
800354A8  lbu   v0, 66(v1)              ; v1 = 0x8009C598
800354D4  sb    v0, 7(s3)               ; ... into the field's HEIGHT
```

It overwrites the three digit fields with the four bytes at `0x8009C5DA`, which are `{50, 0, 250, 0}` — and the
last is the height. **A zero-height sprite draws nothing.** So weapon 1 blanks its own digits by writing a
degenerate rect over them. That address is past the end of the numeral table, inside the unidentified structure
that follows it, so the console is reading bytes that are not numeral records; whether the original meant to
point at a blank cell or merely landed on one is not knowable from here. The effect is what all three retail
captures show — blaster no ammo, shotgun "10", and armour "31" displayed beside a blaster with no ammo readout,
which is what rules out any "hide the counter when the value is zero" reading.

The port models the behaviour rather than reproducing the out-of-bounds read.

**The camera was 286 units too high, and the weapon in the hands is what proved it.** `0x80038618` builds the
view position by copying entity+0x54 and +0x5C straight through and computing the middle component:

```
80038630  lw    v0, 88(a1)      ; entity+0x58, the feet
80038634  lh    v1, 246(a1)     ; entity+0xF6, the eased view offset
80038638  addiu v0, v0, 286
8003863C  subu  v0, v0, v1
```

`q2_sim_eye` had this as `pos.y - view_height` — the same expression with the constant dropped. World Y increases
downward, so the error is a bias in one direction only: the view never looked tilted, just taller, which a
screenshot of a corridor does not betray. `q2_vw_place` carries the expression correctly (`Q2_VW_EYE_BASE`), so
the gun hung off the console's eye while the camera looked from one 286 units above it, and the weapon sat almost
entirely below the bottom edge. `q2psx-inspect viewweapon` never showed it because that path builds its own camera
from the same expression the weapon uses, so both were consistently wrong together and the render looked right.

**Two subsystems that must agree, disagreeing by exactly the constant one of them had dropped** — worth stating as
a pattern, because it is the second time this session that the bug lived in the seam between two components rather
than in either. Anything reading the eye should go through `q2_sim_eye`.

**Found, not yet wired: the weapon strip.** `0x80035EA0` walks the thirteen field records ten bytes at a time and
then draws TWO extra sprites, from rect indices at `s7+96` and `s7+100`, the first skipped when the two are equal.
Their positions come from `0x8009C658` — the head of the structure icontable.h records as "not identified" — which
holds four `s16` pairs: **(388, 201) and (458, 201)** for one player and (381, 95) / (419, 95) for a split. The
y matches the status bar's own anchor row exactly. That accounts for what capture shows: one icon with only the
blaster held, two once a second weapon is picked up. What writes `s7+96`/`s7+100` has not been traced, so the
weapon-to-rect mapping is still missing and nothing is drawn yet rather than guessed.


---

## A session could never leave the map it booted into

`Q2_UF_LOADMAP` — "change level" — has been decoded in `userfuncs.c` for a long time, with its two
operands and the namespace note that goes with them, and **nothing in the port ever acted on it**. A zone
gate was wired (another zone of the same map, deferred to the top of the frame); the level transition was
not. Every session ended where it began.

**It is not a corner the scripts rarely reach.** Sweeping every trigger volume on the disc:

    LOADMAP calls in COMMON: 28; the trigger sweep RUNS 28
      target map is on the disc      : 28  (missing 0)
      start position resolves THERE  : 28, of which land past zone 0 : 12

**28 of 28**, on every count. This is how the game advances.

**Two operand rules are load-bearing and both were already written down.** The start position resolves
against the TARGET map's spawns, not the map the item lives in — 129 of 129 against the target and only
104 of 135 against the container — and a spawn record carries the ZONE it belongs to, so the destination
zone is the arrival point's. **12 of the 28 land past zone 0**, so an implementation that assumed zone 0
would arrive in the wrong part of the level on 43% of transitions, and would look like a working feature
while doing it.

Wired in `client_change_map`, deferred exactly as the zone gate is: a CALL runs inside `q2_sim_advance`,
and loading a map there would free the triggers and the script the runtime is standing in the middle of.

**Shown, not argued.** A demo pad wanders and walks into none of the 28 in three thousand frames, so the
client grew `--fire-triggers` — queue every trigger volume once, through the same runtime path a player
standing in one goes through. The transitions it produces are the game's own order:

    BASE0    -> Base1    zone 0 at Default
    BASE1    -> Base2    zone 0 at FromBase1
    BASE2    -> Jail2    zone 0 at Default
    BASE3    -> Base2    zone 2 at Base2Return
    JAIL2    -> Jail3    zone 0 at Default
    JAIL3    -> Security zone 0 at Default
    LAB      -> Command  zone 1 at FromLab
    WASTE1   -> Waste2   zone 3 at Default
    WASTE2   -> Waste3   zone 0 at Default
    POWER1   -> Power2   zone 1 at Default
    COMMAND  -> Boss1    zone 0 at Default
    SECURITY -> Power1   zone 0 at Default
    BIGGUN   -> Command  zone 3 at FromBiggun

The named arrivals are the tell: `FromBase1`, `FromLab`, `FromBiggun`, `Base2Return` are doorways
authored for a particular approach, and three of the thirteen land in zones 1, 2 and 3.

*Not done here.* The transition is the load, and nothing else: the MISSION screen the console shows
between levels, the inventory that should survive the change, and the intermission's own game-state
request are all still owed. What exists now is that the level ENDS and the next one begins.

- [x] 68. **Walking through a door reset the game.** `client_load_zone` re-inits the sim and `q2_sim_init`
      memsets it, so every transition handed the player back a fresh blaster and 100 health. That is
      obviously wrong for a LOADMAP, and it was equally true of a ZONE GATE — a door inside one level.

      What carries is what `save.c` already treats as the player's rather than the level's: the
      inventory, the weapon in hand, and the chaingun's spin-up count. Everything else in the sim is the
      map's — triggers, script, entity set — and is meant to be rebuilt.

      **The level CLOCK is the awkward one**, because the four powerup deadlines and the mega-health
      bleed are absolute against it. A zone gate stays inside one level, so the clock carries with them
      and nothing needs adjusting. A LOADMAP starts a new level at zero, so a quad picked up at 4,000
      ticks would run for another 4,000 after the door — the deadlines are therefore rebased to preserve
      REMAINING time. That is PC Quake II's behaviour on a level change and it is **stated as the port's
      choice**: nothing in the executable has been read that settles what this console does, and a
      deadline that survives a clock reset unrebased is the one shape that is definitely wrong.

## The weapon strip is drawn, and two of its three numbers are measurements rather than reads

`0x80035EA0`'s two extra sprites are now on screen. What is READ: the two guards (`0x80036188` skips a zero
index, `0x80036198` collapses slot A when both slots name the same weapon), the `index * 5` rect stride, and the
position table at `0x8009C658` — four `s16` pairs, **(388, 201) / (458, 201)** for one player and (381, 95) /
(419, 95) for a split, absolute rather than anchor-relative. That table is the head of the structure
icontable.h had recorded as unidentified.

Two things are NOT read, and are flagged in the code:

- **What fills the slots.** The console takes them from +96 and +100 of a 224-byte record in the array at
  `0x800C7C60` (`s7` is built at `0x80035F08`), and nothing traced so far writes them — `0x800506C4` and
  `0x80050758`, the next/previous-weapon helpers, write +102 and +214 of a different struct. The port derives
  prev/next by walking the owned bitmask and uses the weapon id directly as a rect index. That mapping is
  supported by the sheet — rects 1..11 land on the eleven weapon cells, with slot 6 (hand grenades) sitting off
  the gun row the way an authoring pass would put it — and it produces the blaster icon capture shows. It is
  still an inference.
- **The anchoring.** Drawn with the table's x as the left edge, the icon lands exactly one icon-width right of
  retail: capture puts it at framebuffer columns 427..451 and the table says 458, so 458 is the RIGHT edge. The
  emitter's XY setup at `0x80033320` was not followed far enough to say whether it subtracts the width itself.

Also fixed: the overlay's top line carried a hardcoded `"Quake II"` posted once at startup — a placeholder from
before the overlay had anything real to say. It now names the weapon on a change, and the string is the weapon's
GLYPH (`weapon_glyph[]` at `0x8009DC8C`, "&B"/"&S"/"&U"), which the markup layer expands into a pre-rendered word
out of chars.lbm. "Shotgun" on screen is one sprite, not seven characters, which is why searching for the string
never found it.

**Still open: the view weapon is about two-thirds the size retail draws it.** Measured against the pillarboxed
1500x1125 capture, retail's blaster body spans framebuffer columns 317..419 and the port's spans 280..348 —
1.5x narrower and closer to the screen centre. It is not a projection difference: the horizontal and vertical
distances from centre scale by 1.93 and 1.17 respectively, so no single `proj` change accounts for both. The
placement expression is now confirmed correct against `0x80038638`, so the discrepancy is in the offset, the
model's own scale, or the near-plane treatment of geometry that spans z = 44 to z = 526. Not guessed at.

      **One artefact of `--fire-triggers` worth not chasing.** Firing every trigger volume at once also
      queues zone gates whose payload is only meaningful from the volume they belong to, so a sweep
      reports `no zone 90 in BASE1` and the loader refuses it. A player never stands in every volume
      simultaneously; the warning is the sweep's, not the map's, and the refusal is the correct
      response to it.

- [x] 69. **The MISSION screen had no trigger and no numbers, and both are now the level's own.**
      `mission.h` named its own blocker: *"Kills and Secrets are simulation state, and the sim did not
      tally either — which is the real reason this screen stayed unimplemented long after the machinery
      to draw it existed."* And `main.c`'s briefing key carried the matching note that what shows the
      screen between levels *"is not established"*.

      **LOADMAP is what shows it.** The screen is the level ending, so it is raised where the level ends
      and the destination waits behind it — which is what makes it an intermission rather than a flash.

      **Secrets are `INSECRET`**, a UserFuncs primitive a trigger volume calls. Disc-wide:

          INSECRET calls in COMMON: 34; the trigger sweep RUNS 33

      The total is how many the map's script carries; the found count is how many DISTINCT ones have run.
      *Distinct is the port's choice and is stated:* the runtime fires a volume on entry rather than
      continuously, so pacing in and out of one would otherwise raise the count again, and a secrets
      figure that climbs as you walk about is the one shape that is definitely wrong.

      **Kills come from the creature world** — how many of the map's live-placed creatures are dead —
      rather than from a counter incremented on each death, which would drift the moment a creature were
      removed for any other reason.

      Measured, and both halves move independently. Firing the same transition earlier or later on BASE1:
      `kills 0/8` at frame 980 and **`kills 1/8`** at 1280, either side of the Soldier the `--watch`
      player kills; and the secrets read `2/2` because BASE1 carries exactly two. Across fourteen maps
      every one produces a record — BASE0 `3/4`, WASTE3 `4/4`, BIGGUN `0/0` — and LAB chains two
      transitions in one run, LAB to COMMAND to BOSS1, so the screen releases as well as holds.

      *Still owed:* the console waits for a press and this waits for ESCAPE (headless releases after 45
      frames so a scripted run does not stop at the first boundary). The mission's *unit* number is the
      port's `Mission 1` rather than the level table's, and `"Unit%dMiss1"` — the key that selects a
      level's own display name — is read by nothing yet, so the Location column shows the directory name.

## The 286 belonged to the weapon, not the camera — and a start position is not a standing position

**Correction to the previous entry.** That entry claimed `q2_sim_eye` had dropped the console's 286 and "the
camera was 286 units too high". It was the wrong half. `0x80038638` computes `entity+0x58 + 286 - viewOffset`,
and entity+0x58 is the ENTITY ORIGIN — the point the mover works in, 286 above the feet (sim.h). `player.pos`
holds the feet, so the constants cancel:

```
eye = (feet - 286) + 286 - viewOffset = feet - viewOffset
```

Standing, at 576, that is 576 above the feet: the player's own height. The version committed last round gave
`feet - 290`, half the player's height, and lowered the whole view by 286.

**The crouch case is the tell, not the standing one.** At viewOffset 286 the expression collapses to
`eye = origin` — the middle of the player, which is where a crouched eye belongs. Read with the feet as the
base it collapses to `eye = feet + 0`, an eye on the floor, which is nonsense. A standing-only check cannot
distinguish the two readings (it just makes a shorter player), so the test now pins both offsets.

What was genuinely wrong was `q2_vw_place`, which added 286 to a base that was already the feet and hung the
weapon 286 below the camera. Both now compute `feet - viewOffset` and the gun sits where retail has it.

**A `Population` start position is not a standing position — and this is behaviour-matched, not read.**
Measured against the collision hull, the marker sits above the floor by amounts that vary far too much to be an
author's placement margin: 154 units on BASE1, 225 on BASE2, 612 on BIGGUN, 879 on BOSS1, 990 on BASE0, 1372 on
BASE3, **3814 on WASTE1**. Dropped in under gravity the player visibly falls for the first half-second of every
level. `q2_sim_settle` now resolves the drop before the first frame and clears the fall accumulator, the landing
kick and the view state with it.

It runs the real tick rather than `q2_move_step` directly: a bare move resolves against nothing because a fresh
spawn has not established the entity's node, which is why the first attempt left the player exactly where it
found them. Cost is a dozen ticks of level clock at a level start.

What the console does at a spawn has NOT been traced, so this matches the observable behaviour and says so. If
those marker heights ever turn out to mean something, the spread above is the evidence to explain.

**Still open, unchanged:** the view weapon is about two-thirds the size retail draws it, and no single `proj`
change accounts for it.

## The view weapon was never separately wrong: it and the FOV are one bug

Measuring the reconstruction against the pillarboxed retail capture (game area 1500x1125, so both normalise to
the same 0..1 frame), with everything expressed as a fraction of frame width/height:

| feature | retail | port |
|---|---|---|
| HUD "100" | 0.053 .. 0.187 | 0.047 .. 0.180 |
| HUD cross | 0.197 .. 0.233 | 0.193 .. 0.233 |
| weapon strip icon | 0.833 .. 0.880 | 0.840 .. 0.893 |
| blaster, **height** | 0.205 | 0.195 |
| blaster, **width** | 0.197 | 0.127 |

The HUD is drawn in framebuffer pixels and matches everywhere. The weapon is drawn through the projection, and
its HEIGHT matches while its WIDTH does not — which is not a scale error and not a placement error. It is an
anamorphic one, and it applies to the whole world, not to the weapon:

    ours w/h 0.651   retail w/h 0.961   ratio 0.678

**The factor is exact.** The framebuffer is 512x248 shown on a 4:3 display, so its pixels are not square:

    (512/248) / (4/3) = 1.5484        and        1 / 0.678 = 1.475 measured

`proj` is 160 and the GTE applies one `h` to both axes, giving hFOV 116.0 and vFOV 75.6. A geometrically
correct image at that vFOV on a 4:3 display needs hFOV **91.9**, i.e. a horizontal projection distance of
**247.7 = 160 x 1.5484**. Retail is the correct one; the port is the 116-degree one. The doorway confirms it
independently: retail's centre at 0.407 squashed about the screen centre by 2/3 predicts 0.438, and the port
draws it at 0.434.

So `q2psx-inspect`'s "the console's frustum" framing note is describing an anamorphic frustum as if it were the
finished picture.

**Where the correction cannot go: the GTE.** `gte_rtps` is a bit-exact hardware model with conformance tests
(FIDELITY.md), and the hardware has exactly one `h`. A separate `h` for x would be inventing a register. The
console must therefore carry the aspect somewhere the GTE already reads it — the obvious candidate is the view
ROTATION matrix, whose first row can absorb a 1.5484 scale at no runtime cost, which would also explain why no
second projection constant has ever turned up in the view record. Not yet verified, and the risk to check first
is whether that matrix is also used for anything normal-like, where a scaled row would be wrong.

Consequence worth stating plainly: **there is no separate view-weapon sizing bug to chase.** Once the horizontal
projection is right the weapon comes with it, and the two remaining entries on this list collapse into one.

- [x] 70. **The game plays through, end to end, and three of the mission screen's own fields turned out to
      be sitting in the map's `Strings` chunk unread.**

      `MapTitle` is the level's OWN name — "Outer Base" where the directory says BASE1 — and it is what
      the Location column wants. The level table's `display` is not it: that column reads `Base1`.

      `FoundASecret` is the message `INSECRET` shows, in the map's own words, which is what makes
      "counter++" something the player sees happen rather than a number that changes on a screen they are
      not looking at.

      And the UNIT, which `"Mission %d - Complete"` needs and nothing was reading: a map carries
      `Unit<N>Miss1` for its own unit and no other, and the briefing's key scan already found N and threw
      it away. Recovered, the disc's maps group **exactly as the game's units do**, which is an
      independent check on the reading rather than a restatement of it:

          unit 1  Strogg Outpost, Outer Base, Installation
          unit 2  Detention Centre, Security Complex, Grid Control
          unit 3  Powerplant, The Reactor, Toxic Waste Dump
          unit 4  Research Lab, Defence Command, Gravity Booster
          unit 5  Inner Chamber

      The table now resets when the unit changes, because the screen lists a UNIT's levels in its six
      rows rather than a session's.

      **And the whole chain runs.** `--fire-triggers` re-arms after each transition, so one invocation
      walks the game:

          Strogg Outpost -> Outer Base -> Installation -> Detention Centre ->
          Security Complex -> Grid Control -> Powerplant -> The Reactor ->
          Defence Command -> Inner Chamber -> Boss2

      Eleven levels, five units, a MISSION screen at every boundary and the inventory carried across all
      of them — and by BOSS1 the log reads `weapon 8, weapons 0081`, so a weapon picked up two levels
      earlier is still in the player's hands.

- [x] 71. **The briefing now has its trigger too, and one defect underneath it is exposed rather than
      caused.** `main.c` said of the briefing that *"on the console it is shown between levels by the
      outer state machine; what triggers it is not established"*, and gave it a key. A completed LOADMAP
      is that trigger: the MISSION screen belongs to the level being left and the briefing to the one
      being entered, so the two sit either side of the boundary. Only on a level change — a zone gate
      stays inside one level and has no new orders to give.

      **And the overlay is cleared with the level.** The notifications carry a lifetime on the LEVEL
      clock and a level change restarts that clock, so the first capture of the arrival briefing had
      `You have found a secret.` from the previous map still sitting over the new one's first frames.

- [x] 72. **The briefing's PANEL does not draw, and the buckets are not why.**
      `q2_briefing_build_ot` calls `q2_panel_build_ot` before printing, and the text appears while the
      panel behind it does not — so the briefing is unreadable green-on-orange over whatever wall the
      player arrived facing. It mattered little while the screen was on a debug key; it shows on every
      level transition now.

      The obvious explanation is the mismatch this port already documents: `q2_hud_print` takes a DEPTH
      that is resolved against the ordering table's current window, while `q2_panel_build_ot` takes
      ABSOLUTE buckets through `psx_ot_add_bucket` — gpu.h's own "escape". So the panel's 2 and 1 and the
      text's 0 are not in the same space.

      **Tested, and that is not it.** Passing 0 for all three draws the text and still no panel, so the
      panel is not merely landing behind the world or outside the slice — nothing of it reaches the
      frame. The next thing to check is whether `psx_ot_add_bucket` hands back a primitive at all with a
      window installed, since `q2_panel_body_ot` breaks out of its loop the moment it returns NULL and
      would then emit silently nothing. `q2_briefing_build_ot` is the only caller of the panel anywhere
      in the port, which is why this has never been noticed.

      **FOUND, and it was two bugs stacked, neither of which any test could see.**

      *One.* `psx_ot_walk` draws **bucket 0 FIRST** — far geometry before near — and `psx_ot_add` INVERTS
      a depth on the way in, so depth 0 is the front. `q2_hud_print` takes a depth; `q2_panel_build_ot`
      takes an absolute bucket. The caller's `2, 1, 0` reads as three depths back-to-front and was two of
      them, so the panel went to the two furthest-back buckets in the whole table and the world was drawn
      on top of it. `q2_briefing_build_ot` now maps its two panel arguments through
      `psx_ot_depth_bucket`, which is what makes `2, 1, 0` mean what it looks like.

      *Two, underneath it.* With that fixed the FRAME appeared and the body still did not.
      `q2_panel_body_ot` wrote `xy[0] = position, xy[1] = size` — which is the hardware TILE packet's own
      shape — while `raster_rect` takes the rectangle from `xy[0]` and `xy[2]`. `xy[2]` was never
      written, so the darkening tile rendered as a small box at the top-left corner of the screen. Every
      other rectangle in the port already writes corners: the HUD's sprites, the damage flash, the
      viewport tiles. This was the only one that did not, and it is the only one nothing else called.

      **`tests/test_panel.c` asserted the defect.** It required `xy[1] == size`, because it was written
      from the emitter rather than from the consumer — so emitter and test agreed with each other and
      neither agreed with the rasteriser, and the suite stayed green while the panel drew nothing. The
      assertion is now the rasteriser's, which is the only party whose opinion shows on screen.

- [x] 73. **What a trigger volume ASKS FOR, disc-wide — the list of what is left. CLOSED: the list is empty.**
      `q2psx-inspect zonescript` now prints a histogram of every UserFuncs primitive a player who has
      walked the whole disc would run. It is the first time the remaining work has been a measured list
      rather than an impression, and it is short:

          primitive      in COMMON      run        acted on?
          LIFT1                58       56         no   — the lifts
          CREBATCH             92       82         no   — script-spawned creature groups
          ROTBUTTON            58       53         yes
          STRING               68       33         YES (new)
          SIMPLESOUND          33       33         YES (new)
          INSECRET             34       33         yes
          DONTJUMP             34       33         yes
          LOADMAP              28       28         yes
          INCROUCH             29       27         yes
          HELPCOMPUTER         26       20         no
          ONKEYDO              30       20         no   — the key/inventory gate
          INACID               19       19         yes
          MISEVENT             20       16         no
          ROTHATCH             15       15         yes
          INLAVA               14       14         yes
          INWATER              10       10         yes
          PISTON               13        8         no   — visual-only on PAL (+18 gate is zero)
          TIMER                14        7         no
          UNDERWATER            7        7         yes
          OBJDRAWOFF            7        6         no
          MISCOMPLETE           5        5         no   — end of unit
          TIMEDLIGHT           18        5         yes
          BUTTON, CAGELIFT1, UNDERACID, SIMPLESOUND, TELEPORT, SETWIBBLE,
          DISH, LASERWALL, PLATFORM, FLKLIGHT, DISABLEME, INLOWCROUCH  (1-3 each)
          LASERBEAM            72        0         no   — never trigger-reached
          SIMROT/SIMROT2       22        0         (reached through their own path)
          GLASS                 6        0         wired, never called (#66)

      **The two that mattered most by volume were the movers and the spawns**, and both are done: #75/#76
      for the doors and lifts, #79 for the ambushes. What the histogram now leaves, in order of runs:

          MISEVENT      16   namespace UNLOCATED — 0 of 93 resolve against Strings
          TIMER          7   delayed continuation of the rest of a record
          MISCOMPLETE    5   end of unit
          CAGELIFT1      3   names no speed (#81)
          DISH           2   names no speed (#81)
          LASERWALL      2   a damage volume
          PLATFORM       1   names neither target nor speed (#81)
          DISABLEME      1   sets DISABLED on the running record
          LASERBEAM     72   in COMMON and reached by NO trigger volume

      Everything else on it is acted on. `LASERBEAM` is the odd one: 72 items and a trigger volume runs
      none of them, which is the same shape as GLASS (#66) and means the same thing — it is reached from
      somewhere other than the trigger graph.

      **Answered by #89, and the answer is that nothing is supposed to reach it.** A LASERBEAM is lit by
      the bottom bit of its own first coordinate, carried in the ZONE's copy of the script, and the zone
      load's registration pass is what raises it. Zero trigger runs was never evidence of a gap.

      **CLOSED.** Every entry on the "what is left" list above has since been resolved, and it is worth
      keeping the list beside its answers because the shape of the answers is the finding:

          MISEVENT      the namespace was not a chunk at all — a name+handler table (#93)
          TIMER         a delayed continuation of the rest of the record (#83)
          MISCOMPLETE   ends a UNIT, and reading it named Q2_SCREEN_EXIT_7 (#87)
          CAGELIFT1     LIFT1's constructor operand for operand; the table was short a row (#82)
          DISH          the speed is an immediate 1, not an operand (#120)
          LASERWALL     0 armed of 2 declared: both name an empty object slot (#117)
          PLATFORM      its target is the distance from its node's own box centre (#82)
          DISABLEME     retires the record it runs in; the one reachable call is behind the key gate (#119)
          LASERBEAM     nothing is supposed to reach it (#89)

      Three of the nine turned out not to be gaps at all — LASERBEAM is reached from the zone load,
      LASERWALL's two calls cannot fire on this disc, and DISH's missing speed did not exist. That ratio is
      the reason the histogram was worth building: it is a list of things to LOOK at, and looking is what
      distinguishes a missing caller from a primitive that is already doing what it should.

- [x] 74. **STRING and SIMPLESOUND — the game speaks and makes noise.** Both were decoded long ago and
      neither was acted on. A trigger volume runs 33 STRING calls and all 33 SIMPLESOUND calls disc-wide,
      and the text is the map's own: JAIL2 says `Yellow Lasers deactivated`, LAB says `Access granted`,
      `Maintenance bridge unlocked`, `Level Two security disabled`, `Level Three security disabled`, and
      WASTE3 says `Find and activate both coolant pumps.`

      A STRING key with no text is silence rather than a warning, because `userfuncs.c` already records
      that a miss is normal — 165 of 363 uses resolve disc-wide. SIMPLESOUND's absolute world position is
      **not** used: this port's mixer has no positional path, so the sound plays flat, and that is stated
      at the site rather than left to be discovered.

## The world was anamorphic, and the way to measure it is a rigidly-attached object

Applied, after two earlier attempts got the sign wrong by eyeballing screenshots. The method matters more than
the number, so it is recorded first.

**Calibrate, do not assume the capture's aspect.** The reference is a DuckStation capture at 640x480. Rather
than guessing what it was displayed at — an assumption that produced a 4:3 reading, a 1.486 reading and a 1.45
reading on three different attempts — solve the framebuffer-to-image mapping from the **HUD**, which is drawn in
framebuffer pixels at coordinates the executable states. The cyan numerals give x scale 1.2809, y scale 1.8571,
origin y 10. Everything downstream is then exact and no longer depends on what the emulator chose.

**Measure something bolted to the camera.** A first attempt used the canyon's sky gap and concluded the opposite,
because the reference frame was taken after the player had walked forward, so every world feature was nearer.
The blaster's muzzle stripe cannot move relative to the camera, so it isolates the projection.

**Sample the whole idle cycle.** The weapon sways, so one frame proves nothing. Over 22 frames:

| | stripe w/h, median | range | median area |
|---|---|---|---|
| before | 0.67 | 0.41 .. 0.81 | 90 |
| after | 0.85 | 0.64 .. 1.19 | 147 |
| retail | 1.00 | — | 139 |

Before, the stripe never once reached retail's WIDTH while its height comfortably exceeded it — a horizontal
deficiency, not a scale error, and the shape of the evidence that says "anamorphic" rather than "too small".

**The fix.** `gte_rtps` computes both axes from one `h`; that is the hardware and there is no second register.
So a 512 x 248 buffer on a 4:3 screen is anamorphic — hFOV 116.0 against vFOV 75.6, where a correct picture at
that vFOV wants 91.9 — unless view-space x is scaled before the divide. The only thing upstream of `ir[0]` is
the rotation matrix, so the factor goes in its first ROW, which costs nothing at run time and explains why no
second projection constant has ever appeared in the view record:

    (512/248) / (4/3) = 1.5484        measured shortfall 1/0.67 = 1.49

Derived from the viewport rather than baked in, and taken from the projection CENTRE the camera already carries,
so a split viewport gets its own correction rather than the whole frame's.

After it, the weapon's area agrees with retail to within 6% where it was 35% under, and retail's shape ratio
falls inside our idle range. The residual world differences in a side-by-side are player position: the reference
is a video frame taken after the player moved.

**Not applied to `effect.c`**, whose two `gte_set_rotation` sites were not audited — particles will be narrow
until they get the same treatment.

- [x] 75. **Every door and lift on the disc stood still, and the mover module has been finished the whole
      time.** `mover.[ch]` carries the seven-state machine, all three payload shapes, the key gate and the
      displacement the zone draw already adds through `q2_movers_node_offset` — and `q2_movers_build`,
      `q2_movers_tick` and `q2_mover_trigger` had **no callers anywhere in the port**. 1,006 `MOVER_A`
      items, 20 `MOVER_B` and 292 `MOVER_C`, none of them ever asked to move.

      It is the same shape as the rotators before #56 and the same fix: the runtime reports a MOVER item
      and the OWNER says which mover that is, because the set does not live in the runtime.

      **The identity is the item's chunk offset**, stamped on every mover the builder pushes. An ordinal
      would work only while build order and execution order agree and nothing guarantees they do; and it
      has to be per-ITEM rather than per-mover because `MOVER_C` builds two movers from one item — the
      two leaves of a double door — and a script that opens the door means both.

      **`events_rt.c`'s note was right about the hazard and wrong about the conclusion.** It called the
      MOVER opcodes "deliberately inert" because "the s16 slots hold Scene node indices on disc and
      runtime object indices after a load-time pre-pass that has not been decoded". The disc values are
      Scene node indices, and `mover.h` deliberately reads them as such rather than reproducing the
      console's rewrite — so acting on them was correct all along.

      Measured across seventeen maps, firing every trigger volume: BASE0 8 movers / 263 tick-moves,
      LAB 34 / 442, COMMAND 10 / 982, POWER2 34 / 579. And from an ORDINARY demo walk with nothing
      staged, LAB opens one door and moves it for 120 ticks — a player walking into a volume, which is
      the path that matters.

      **The picture is not subtle.** At LAB frame 780 the two runs differ by 94,068 pixels of 126,976:
      with the movers inert the player is inside the closed door looking at its back face, and with them
      working the door is open and they have walked through it. A closed door is a wall.

## RETRACTION: the horizontal projection correction, and why the measurement was wrong

Two commits back the world's horizontal projection was scaled by 1.5484 on the grounds that the port was
anamorphic against retail. **Both are reverted.** screen.h had already settled this, in terms that name the
mistake in advance:

> the console's own image is horizontally compressed by a factor of 1.5. That is not a defect in this
> reconstruction and it must not be "corrected": there is no anamorphic term anywhere in the image to undo it.

That is a search of the transform chain — the only matrix scale on it is the uniform (768,768,768) at
`0x800AEB30`, an object scale applied per column by `0x80055AF8`, and the projection distance reaches the GTE
untouched from view+262 — corroborated by the 2D art, whose menu letterforms are authored about 1.35x wider
than tall and only read correctly once the television has narrowed the pixels. A code-level finding beats a
pixel measurement, and this file had it written down the whole time.

**Where the measurement went wrong, because the method looked sound.** The claim rested on the blaster's muzzle
stripe: rigidly attached to the camera, therefore independent of where the player is standing, measured across
22 frames of idle at a mapping calibrated from the HUD. Every one of those steps was a genuine improvement on
the eyeballing that preceded it — and the conclusion was still wrong, for a reason none of them addressed.

The weapon is camera-locked **only for a given animation pose**. The reference is a frame from a video at 2:43
with the player moving; the port's sweep was of a stationary player's idle cycle. The stripe sits on an angled
face, so a pose difference of a few degrees changes its projected width by more than the factor being measured.
"Rigidly attached" was doing work it could not do.

The calibration actually argues the other way and it was not read that way at the time: solving the
framebuffer-to-image mapping from the HUD gives x 1.2809 and y 1.8571, a display pixel aspect of **1.4498** —
which IS the television narrowing the pixels. The display already undoes the compression. That number was
measured, written down, and its meaning missed.

**What would settle it**, if anyone wants to close it properly rather than trust the code reading: a retail
capture at a level start, stationary, with the weapon settled in idle. That is a pose the port reproduces
exactly and deterministically (`--headless --frames 90`), so the confound disappears. Every reference used so
far has been mid-play.

**Still open, and now with no proposed cause:** in a side-by-side at the calibrated mapping the port's weapon
looks smaller than retail's. Scaling the projection made that look better, which is exactly why it was
convincing, but a change that improves one object's appearance while contradicting the transform chain is a
coincidence and not a fix. The real cause is unfound.

- [x] 76. **The other half of the lifts: the ones a CALL builds.** `MOVER_A/B/C` are opcodes in the record
      stream; `LIFT1` is a CALL primitive that builds the same kind of runtime object, and it accounts for
      56 of the runs in #73's histogram. `userfuncs.c`'s operand table maps one onto the other exactly —
      `param_a` is the target negated, `param_b` the speed, `objects[4]` the Scene nodes, `time_a`/`time_b`
      the delay and the wait — so `q2_movers_build_calls` appends them to the same set the opcodes build,
      and they share a tick, a draw offset and a trigger. The object slots take the #56 rebase, as a
      rotation call's do.

      A LIFT1 call is both the constructor and the trigger: the same item that built the mover is the one
      that asks it to move.

      Per map: LAB 34 opcode movers plus **4 lifts**, POWER2 34 plus 4, COMMAND 10 plus 2, BASE0 8 plus 2.

      **CAGELIFT1 is deliberately not built** — three calls on the disc, and its operand table names no
      speed. A mover with speed zero is one that is triggered, ticks and never arrives, so it needs its
      constructor read rather than a constant borrowed from `LIFT1`.

## CLOSED: the view weapon "looks smaller than retail"

Carried as an open gap for three rounds and used to justify a wrong change to the projection. It is closed as
**not established as a defect**, and the reason is that every instrument built to measure it was confounded.

**What the port can be held to, and passes.** `q2psx-inspect viewweapon` checks the weapon against the
executable rather than against a screenshot: the four key-field offsets, the 70-tick switch countdown, the
286-unit eye base at `0x8004F608`, the state machine's transitions, and placement — that the weapon drops with
the eye by exactly `576 - 286`, that a quarter turn moves it, and that its distance from the eye is unchanged
by turning. **20 of 20 pass.** The translation it is placed with is the animation bank's own `t`, constant at
(140, 157, 44) across the idle, and the model is the CastList's own geometry. There is no free parameter left
in which a size error could hide without one of those checks failing.

**Why the visual impression cannot be promoted to a defect.** Three separate attempts to measure it:

- *Eyeballed bounding boxes*, twice, which disagreed with each other on the direction.
- *The sky gap*, which is a world feature and therefore moves with the player — the reference frame was taken
  after the player had walked forward, so everything in it is nearer.
- *The muzzle stripe across 22 idle frames at a HUD-calibrated mapping.* The best of the three, and still wrong:
  the weapon is camera-locked only for a fixed animation POSE, and the reference is mid-play at 2:43 of a video
  while the sweep was a stationary idle. The stripe sits on an angled face, so a few degrees of pose changes its
  projected width by more than the effect being measured.

A fourth attempt, segmenting the arm as a pose-robust proxy anchored at the fixed grip, failed outright — a
tan arm against tan rock does not segment, and the "measurement" returned the search window's own bounds.

**What the code says.** screen.h's search of the transform chain found no anamorphic term to undo, and the HUD
calibration independently measured the display pixel aspect at 1.4498 — the television narrowing the pixels,
i.e. the display already performing the correction the projection was accused of missing.

**What would reopen it**, for anyone who wants certainty rather than the absence of evidence: a retail capture
at a level start, stationary, weapon settled in idle. The port reproduces that state exactly and deterministically
with `--headless --frames 90`, so the pose confound disappears and a single comparison settles it. Until such a
frame exists, "the weapon looks smaller" is an impression from mid-play footage and not a finding.

- [x] 77. **ONKEYDO: every gated script in the game ran for free.** It is a PREDICATE — it tests the
      player's key bits and, when they do not satisfy it, aborts the rest of the record it sits in — and
      nothing acted on it. That was invisible while the things those records gate did nothing, and stops
      being invisible the moment the same records open doors: **JAIL2's mover triggers fall from 51 to
      46** once the gate is honoured, which is five doors that used to open without the key.

      The abort is a FLAG rather than a return value, because that is what the console does — `gp[0x423C]`,
      set by the primitive and read by the record executor — and because an `on_call` hook that only
      reports a CALL has nowhere to put an answer. `q2_event_rt.abort_record` is that flag; the executor
      clears it and still marks the record as having run, exactly as the console's does, or a locked door
      would re-test on every touch.

      The four tests are `userfuncs.c`'s and a zero operand disables its own test rather than requiring
      nothing to be set. Gated records per map with no keys held: JAIL2 5, LAB 5, POWER2 3, COMMAND 3,
      BASE2 2, WASTE2 1.

      *Note on the measurement:* `SIMPLESOUND` reports 0 played in every headless run, and that is the
      absence of an audio device rather than of the wiring — `client_play_sound` needs one.

- [~] 78. **OBJDRAWOFF is wired and its operands read -1, which is a measurement rather than a failure.**
      `flags08` bit 15 is the hide flag and the zone draw has honoured it since the surface pass; it is
      clear on every node on the disc because this primitive sets it at RUN TIME, on the runtime object.
      This port does not build the console's 48-entry object array (mover.h says why), so the runtime
      state lives beside the zone — `q2_world_zone.node_hidden`, one byte per Scene node, owned by the
      client and checked by the draw next to the chunk's own bit.

      Only four maps carry the primitive at all: BASE2 1, JAIL4 4, JAIL5 1, SECURITY 1 (of which
      SECURITY's is never reached).

      **On JAIL4 all four calls ARE reached and all four read `-1 -1 -1 -1`**, through the #56 rebase and
      not merely from COMMON's copy.

      **And it is not a resident-zone problem, which was the obvious guess.** #56's other half — the
      engine holds one zone resident, so a call barren under zone 0 may be live under zone 3 — was tested
      here and does not apply: JAIL4 under **all four** of its zones and BASE2 under all three give
      `0 nodes hidden`. The operand is -1 in every buffer the map ships.

      So the slots for these calls are genuinely empty on this disc, and OBJDRAWOFF's runtime object must
      be getting its node from somewhere other than the item — which is the same shape as the `rest`
      field and the mission counters: a field that is runtime state rather than authored data. The
      mechanism is wired and will work the moment something puts a node in it; what is unread is who
      does.

- [x] 79. **CREBATCH: every scripted ambush in the game was already standing in the room.** #73 called
      this blocked on `LevelBin` and that was too quick. The question it turns on is what the calls NAME,
      and that is measurable:

          CREBATCH calls in COMMON: 92, naming a group that exists: 89
            the group claims a ZONE (the level's own population) : 31
            the group claims none (a batch a script summons)     : 58

      **58 of 89 name a group claiming no zone**, and the names say what they are: `ShotgunRoom`,
      `LiftRoom`, `BerserkHide`, `InTheRoom`, `Upstairs`, `MainRoom`, `KeyConsole`, `batchx`, `batch3`,
      `Z0Bat0`. A group named `Zone<N>` is that zone's own population and stands there from the start;
      anything else waits to be called for. `population.h` had already established that split from the
      other side — 74 of 222 groups are named after a zone, one per zone a map ships — so the two
      readings agree without either being tuned to the other.

      That is what stands in for the LevelBin selection this port cannot run, and it needs no LevelBin.

      **For creature spawn records, the port spawns every record and holds the batches DORMANT** rather
      than re-running the module-owned monster setup mid-level. Population *place* records now take the
      other safe path: only the resident/LevelBin-selected item groups exist at startup, and CREBATCH
      appends a deferred place group when called. Both paths honour the bit-1 one-shot latch.

      Measured across twenty maps, every one of them summons — BASE1 19 creatures, JAIL2 31, JAIL4 26,
      BASE3 24, JAIL3 25. And BASE1 now starts with **4 live where it started with 8**, the other four
      arriving when the script asks for them.

- [x] 80. **HELPCOMPUTER, SETWIBBLE and TELEPORT.** Three more from #73's histogram, and the first is the
      one a player meets: 20 of the disc's 26 `HELPCOMPUTER` calls are reachable by a trigger volume, and
      the two Strings keys each carries are the game's own objectives —

          BASE1  "Locate Base Installation Elevator."
          BASE2  "Use sewer tunnels to gain access to the Comm Centre."
          JAIL4  "Get a Security Pass from the Cell Blocks to gain further access."
          LAB    "Locate the Repair Facility and steal the Commander's head you find there."
          POWER1 "Locate the Reactor Core and disable its safety mechanism."

      — none of which had ever appeared. Its third operand selects a screen this port does not have; the
      text goes to the overlay, where the port's own notifications go.

      `SETWIBBLE` writes the low four bits of its operand into `flags08` bits 10..13, and bits 10-11 are
      the DRAW VARIANT: variant 3 links nothing, which is the second way a script hides a surface group.
      Only that case is acted on — the other three variants are subdivision choices this port makes per
      quad rather than per node. Its operand is a Scene NODE index and takes no rebase, which
      `userfuncs.c` is explicit about: the constructor only restores bytes, it never rewrites them.

      `TELEPORT` resolves 28 of 28 against the map's own spawns, and the zone switch **is** implemented:
      "switches zone first if the target is in another one, then sets entity position" is a zone load
      with an arrival point on the end, which is the zone gate's own path. Queued to the top of the frame
      like every other transition, because the CALL raising it runs inside the script a zone load frees.

      Only BIGGUN carries any — two — and in the client both sit behind `ONKEYDO`, so the demo never
      takes them: you cannot teleport without the key. The tool's sweep runs 2 of 2 because it does not
      honour the gate, and the difference between the two numbers is the gate working rather than the
      teleport failing.

- [x] 81. **BUTTON and PISTON join the movers, and the PISTON pusher is gated.** Both name a
      target AND a speed, which is what a mover needs — a button's speed is literally one unit a tick
      (`travel`'s sign selects `obj+0x3A` = +1 or -1 and its magnitude goes to `obj+0x44`). PISTON's
      constructor copies its low-two-bit axis at `+4`, speed at `+5`, signed target at `+6`, and all FOUR
      object slots at `+8/+10/+12/+14`. Its signed `+18` word decides whether it creates `obj+0x28`, the
      pusher whose per-frame call reaches the shared carry/rollback path. A pusher first tries to carry a
      blocker, restores it when the carry cannot finish, and then calls
      `T_Damage(NULL, blocker, 30, MOD_CRUSH, NULL)` at `0x80051E74` before the normal blocked handler sees
      the veto. **Every one of the thirteen PAL PISTON calls has `+18 == 0`**, so none creates that pusher:
      they animate geometry only and are not solid crushers.

      **PLATFORM and DISH are not built**, for the same reason as CAGELIFT1: PLATFORM names neither a
      target nor a speed and DISH names no speed. A mover with speed zero is triggered, ticks, and never
      arrives, so all three need their constructors read rather than a constant borrowed from a sibling.

      The builder now DROPS a mover with no node or no speed rather than keeping one that cannot move,
      and logging what it dropped is where the useful measurement is:

          POWER2  PISTON built, 2 nodes — visual actuator, +18 pusher gate clear
          JAIL4   BUTTON rejected, 0 nodes — its object slot reads -1
          LAB     4 LIFT1 rejected, 0 nodes; 2 built
          BASE0   1 LIFT1 rejected with FOUR nodes and speed 0

      So the OBJSLOT emptiness of #78 is not confined to OBJDRAWOFF: BUTTON has it too. But it is not
      universal either — PISTON's first two slots resolve, and the rotators' resolve 41 of 95. Whatever leaves an
      object slot empty is per-primitive or per-item rather than a property of the format, and BASE0's
      LIFT1 shows the same thing on the OTHER operand: four good nodes and a speed of zero.

- [x] 82. **CAGELIFT1 has a speed; the operand table was short an entry.** #81 declined to build one
      because its table named none, and the table was wrong rather than the primitive.

      Its constructor at `0x80029794` is LIFT1's, operand for operand. With `s0 = obj + 0x38` — pinned by
      `sw a3, -12(s0)` writing the per-frame tick `0x80025658`, which `mover.h` already records both
      lifts install:

          800298BC  lhu  v0, 4(s5)      ; item +4
          800298C4  subu v0, zero, v0   ; negated
          800298C8  sh   v0, 12(s0)     ; -> obj+0x44, the TARGET
          800298CC  lh   v0, 6(s5)      ; item +6
          800298D4  bgez / subu         ; abs()
          800298E0  sh   v0, 2(s0)      ; -> obj+0x3A, the SPEED

      Built now, and it builds: BASE2 goes 1 -> 2 CALL movers, JAIL4 0 -> 1.

      **And PLATFORM's table was wrong in the same way and in a more interesting place.** Its speed is at
      `+18` (`0x8002CDC4`), which the table also omitted; and its `origin` is not "read by the
      constructor only" — the TARGET is computed FROM it. `0x8002CCE8` sums three squared deltas and
      passes them through `0x80055CBC`, so a platform's travel is the distance between the node's own
      position and where the script says it ends up. That is why this port still does not build one: the
      target needs the node's position, which the mover builder does not have. One call on the disc.

      `DISH` remains unread — its constructor writes only `obj+0x3E` from the operands, so its speed
      comes from somewhere this pass did not find.

- [x] 83. **TIMER: the rest of the record, later.** "A delayed continuation of the rest of the record" —
      the items before it run now and the ones after it run when the delay expires. The runtime had no
      way to express that, because a record either ran to its end or was aborted.

      Expressed the way the abort is, as a field an `on_call` hook writes (`q2_event_rt.defer_ticks`),
      because the hook reports a CALL and has nowhere else to put an answer. A non-zero value stops the
      record and queues its remainder against the runtime's own clock; `q2_event_rt_advance` moves that
      clock and `q2_event_rt_update` runs whatever has come due BEFORE it takes anything new off the
      queue — so a timer that expires runs on the frame it expires rather than behind whatever the player
      has just walked into. A deferred record is not latched as having run until it finishes.

      The delay is `(base + ((range * rand()) >> 15)) * 30`, and **the 30 is not the 300** every other
      time on this clock uses; `userfuncs.c` calls that out and it is the sort of thing that is silently
      four-fifths wrong if assumed. The RNG is the sim's rather than the BIOS's — a stated divergence,
      so a timer's jitter is the right shape and not the same sequence.

      Measured on LAB, whose two timers read `base 50, range 0` — 1,500 ticks, five seconds. From a
      trigger at frame 60: **0 resumed at frame 200, 1 at 260, 2 at 400**, which is the deadline landing
      where the arithmetic puts it.

      *An artefact worth knowing:* in an ordinary `--fire-triggers` sweep the timers never resume,
      because the same sweep fires the map's LOADMAP and the level ends four seconds before the deadline.
      The measurement above suppresses the transition. A player who walks into one volume rather than all
      of them does not have that problem.

- [x] 84. **PLATFORM is read in full, and its slot is empty like the others — which is now a pattern, not
      an accident.**

      The target is the DISTANCE from the node's bounding-box CENTRE to the `origin` operand.
      `0x8002CC98`..`0x8002CCE8` reads the node record's `+16/+28`, `+20/+32` and `+24/+36` — the two bbox
      corners — halves each pair, subtracts it from the item's VEC3 at `+4`, squares and sums; then
      `(sum >> 2)` goes through the integer square root at `0x80055CBC` and the result is doubled
      (`sll s4, v0, 1`). `isqrt(n/4) * 2` is `isqrt(n)`: the halving keeps the intermediate in range and
      is not part of the answer. The axis is Y — `and 0xFFFF3FFF` then `ori 0x4000` puts 1 in `obj+0x50`'s
      two-bit axis field, the same write CAGELIFT1's constructor makes.

      So a platform's travel is not authored as a length. It is the gap between where the node IS and
      where the script says it ends up, which is why `origin` looked like it was "read by the constructor
      only".

      **And the one PLATFORM on the disc reads `slot -1 speed -4`.** The speed is real; the object slot
      is empty — the third primitive to do that, after `OBJDRAWOFF` (#78) and `BUTTON` (#81).

- [x] 85. **What actually fills an object slot — ANSWERED. A slot is PER ZONE, and −1 is the right answer
      in every zone the object is not in.**

      The question assumed a slot reading −1 was an unfilled handle. It is not. `OBJDRAWOFF`'s constructor
      at `0x8002BD58` is four instructions of substance and they say so outright:

          8002BD90  lw    v0, 372(gp)      ; the working chunk
          8002BD94  lw    v1, 376(gp)      ; the pristine one
          8002BD98  subu  v0, s1, v0       ; s1 = item + 4
          8002BD9C  addu  s0, v0, v1
          8002BDA0  addiu v0, zero, -1
          8002BDA4  sh    v0, 0(s1)        ; WORKING SLOT := -1, unconditionally
          8002BDA8  lh    v0, 0(s0)        ; and the authored index comes from PRISTINE
          8002BDB0  bltz  v0, skip         ; negative there means "no object here"
          ...
          8002BE60  sh    a0, 0(s1)        ; the allocated object's index, written back

      So −1 in the working buffer is not a missing value: it is **what the constructor puts there**, and
      the authored Scene node index lives in the other buffer. That is #56's shape again, in a field #56
      never looked at.

      Measured across the disc — every relevant OBJSLOT (all four for PISTON), in COMMON and in every zone:

          85 items;  usable in COMMON : 26,  usable in some zone : 81,  nowhere : 4

      and the per-item picture is unambiguous. **Each object exists in exactly one zone:**

          JAIL4  OBJDRAWOFF x4   COMMON -1, zones 0/1/2 -1, zone 3 -> 203, 196, 189, 182
          BASE2  OBJDRAWOFF      COMMON 215, zone 0 215, zones 1/2 -1
          JAIL5  OBJDRAWOFF      zone 2 -> 46
          SECURITY OBJDRAWOFF    zone 2 -> 21
          BIGGUN PLATFORM        zone 2 -> 31
          BASE2  BUTTON          zone 2 -> 278
          JAIL2  BUTTON          zone 2 -> 96

      **So nothing was broken.** The port already reads the resident zone's copy, and it already acts on
      it: JAIL4 zone 3 reports `4 nodes hidden` and zones 0, 1 and 2 report none, which is exactly the
      four slots that resolve there and nowhere else. BASE2 zone 0 and JAIL5 zone 2 each hide their one.
      The earlier reading — "OBJDRAWOFF, 4 of 4 on JAIL4, under ALL FOUR zones" — was measuring something
      that was answering correctly.

      Two corrections fall out of having measured it properly. BUTTON's slot is at **+12** and PLATFORM's
      at **+20**, not +4; a census that reads +4 for all three is reading BUTTON's invert flag and the
      first word of PLATFORM's origin, which is how those two came to be on the broken list at all.

      **Four items resolve in no zone, and those are dead script**: COMMAND's two PISTONs and one LIFT1,
      and JAIL4's BUTTON. A primitive declared with no object is something the level author left behind,
      and the console does the same nothing with it that this port does — `bltz` and skip.

      This also closes the loop with #89. A LASERBEAM is lit by a bit in the zone's copy of a coordinate;
      an object slot is filled by an index in the zone's copy of the slot. **Which things exist is a
      property of the room you are standing in, and it is carried in the zone's script, not in COMMON's.**
      That is one mechanism, and it has now been mistaken for a bug twice.

- [x] 86. **MISCOMPLETE ends a UNIT, and reading it names `Q2_SCREEN_EXIT_7`.**
      `0x8002DC68` is four instructions of substance: copy the fixed string `"Default"` into the
      arrival-point buffer at `0x800C8CD0`, and write **7** into the game-state word at `0x800B2E28`.

      `screen.h` had exit 7 listed by number with no name. `0x80018ED8` names it: it tears the level's
      two module images down (`0x8006D280` on `0x800CBA28` and `0x800CBD28`), runs the outer state
      machine at `0x80018868` until it answers, and then either loads **`"Extro FMV"`** on answer 5 — the
      ending — or **`"EndMission N"`** with the digit at index 11 patched from `0x800B2E20`
      (`lbu 11(a0)` / `addu` / `sb 11(a0)` at `0x8001900C`..`0x80019028`). Those are the `QENDMIS1`..
      `QENDMIS5` maps the level table carries as `EndMission 1`..`EndMission 5`.

      Wired, and it resolves by DISPLAY name because `"EndMission N"` is one — which also gives
      `q2_level_find_display` its first caller. The four maps that carry a MISCOMPLETE each go to their
      own unit's screen, and the unit numbers recovered from `Unit<N>Miss1` (#70) land exactly right:

          SECURITY  unit 2 -> EndMission 2 (QENDMIS2)
          POWER2    unit 3 -> EndMission 3 (QENDMIS3)
          COMMAND   unit 4 -> EndMission 4 (QENDMIS4)
          BOSS2     unit 5 -> EndMission 5 (QENDMIS5)

      That is an independent check on the unit reading: nothing in #70 was tuned to produce it.

      **The choice between `EndMission N` and `Extro FMV` is the port's**, made from the unit the map
      declares rather than from the outer state machine's answer, because that machine is not
      reconstructed. Stated because it is the one invented step; unit 5 is the last on this disc.

      *And the destination is a stub in this port.* `QENDMIS2` loads as 2 quads and 2 nodes and draws
      nothing, because its content is its `LevelBin` module's — the same blocker as #85, #6 and
      CREBATCH's initial selection. The transition is real; what is on the other side of it is not yet.

- [x] 87. **The LevelBin's group selection, read without running the module — and #79's rule turns out to
      be the module's own.**

      `population.h` states the blocker: a group is spawned because a script SELECTED it, the flags word
      is zero on disc for all 222 groups of all 49 maps, and *"which groups a level selects therefore
      lives in its `LevelBin`, which this port does not run"*. #79 approximated it by a naming rule. The
      module can be READ instead, which is what `q2_fx_glint_scan` already does for the glint.

      **The call, from BASE1's zone handler at module+0x94:**

          801000A0  addiu a3, v0, 12     ; a3 = module + 0xC = "Zone0"
          801000A4  lbu   v1, 1(a3)      ; ...and twenty more
          ...       lw    vX, 36(rY)     ; the engine block's slot 9
          ...       jalr  vX

      A twelve-byte name arrives BY VALUE in a0/a1/a2, assembled byte by byte from a pointer the compiler
      picked — which is why a first attempt that looked for `addiu a0` found 21 call sites and **zero**
      real group names.

      **Which slot is the selector was SWEPT, not assumed.** For every offset a module calls with a
      name-shaped argument, count how many of those names are groups the map ships:

          +36    71 / 83      <- the selector
          +136    4 / 5
          +28     1 / 20
          +32     1 / 20
          everything else  0 / n

      One offset accounts for essentially every hit and the rest for none. `36 = 4 * 9`, so FORMATS §15.5's
      slot numbering indexes the block directly — slot 0 IS the size word — and an earlier reading here of
      "4 + 4*N" was wrong.

      **And the answer confirms #79 rather than replacing it.** Of the 71 selections that resolve, **69
      claim a zone**. The two that do not are JAIL3's `Jail4Return` and `Jail5Return` — populations for
      coming BACK to a level, selected conditionally, and now reachable because the client takes the
      module's list first and falls back to the naming rule only where the module says nothing. No
      CREBATCH names either of them, so before this they could never have spawned at all.

      Also settled by the string scan alongside it: every group a level uses is named in its LevelBin —
      75 groups across 21 maps — but a mention is not a selection. JAIL3's `Bridge` is named by the module
      AND by a CREBATCH, which is what forced the call decode rather than the string search.

      The decoded selection is now shared by **items as well as creatures**. `q2_item_spawn_zone` runs the
      resident `Zone<N>` and explicitly selected place groups, while `q2_sim_activate_item_group` handles a
      later CREBATCH once. Version-5 saves carry an `ITEM` chunk with exact group activation order and stable
      Population `(group, place-slot)` keys, so a collected startup slot reused by a deferred item restores
      to the same physical entity slot instead of failing the old entity-count check.

- [x] 88. **The campaign runs end to end.** Five maps carry a `MISCOMPLETE` and it is **one per unit, on
      that unit's last level**: BASE2 ends unit 1, SECURITY 2, POWER2 3, COMMAND 4, BOSS2 5. Nothing was
      tuned to produce that; it falls out of the unit numbers recovered from `Unit<N>Miss1` (#70) meeting
      the five calls.

      A unit's last level therefore carries BOTH a LOADMAP and a MISCOMPLETE — BASE2 has three of the
      former — and a player fires one of them by walking into one volume. `--fire-triggers` fires every
      volume at once, so within that artificial batch the ordering means nothing and the first writer
      wins; otherwise the unit end clobbers the level exit and a scripted run stops at BASE2.

      With that, one run walks:

          BASE0 -> Base1 -> Base2 -> Jail2 -> Jail3 -> Security -> Power1 ->
          Power2 -> command -> Boss1 -> Boss2 -> QENDMIS5

      twelve levels, five units, ending on the last unit's own end-of-mission screen.

- [x] 89. **LASERBEAM — the beams a level ships, and the sort that was hiding them.** 72 items, zero reached
      by a trigger volume, and the shape looked exactly like GLASS (#66). It is not that at all. Nothing is
      supposed to reach a LASERBEAM: **which beams burn is a property of which zone the player is standing
      in, and it is carried in the bottom bit of a coordinate.**

      The constructor (`0x8002E718`) runs at every zone load and takes word 0 of each endpoint from the
      OTHER buffer — the zone's own Events chunk, the #56 rebase. The exec (`0x8002E694`) then tests bit 0
      of that word (`andi v0, v0, 1` at `0x8002E6C0`) and, if it is set, appends `{raiser, item}` to a
      32-entry list at `0x800C7014`. A per-frame walk (`0x8002EE38`) re-submits the whole list for ever.

      JAIL2's corridor grid is `X=7352` in COMMON and in zone 0, and `X=7353` in zones 1 and 2 — the same
      coordinate with the bottom bit set, one unit wide of nothing. Disc-wide: **71 of the 72 beams are lit
      in at least one zone, none is lit in every zone**, and the one lit nowhere is JAIL2's
      `(0,0,0)->(0,0,0)` dead entry. Reading COMMON's copy alone calls 41 of 72 dark and is simply the wrong
      buffer — the same mistake, in the same field, that #56 was about.

      Two operand labels were wrong and are corrected in `userfuncs.c`. `+18` is not an OBJSLOT: the
      constructor overwrites it with `q2_coll_find_node(PrimaryColl, &origin_a, hint -1, brute)`, so it is
      the beam's **area**. `+34` is not a counter: it is the laser **kind**, clamped by the constructor to
      the same six the dispatcher accepts (`sltiu v0, v1, 6`) — and all 72 disc beams are already in range,
      which is what a kind field looks like and what a counter would not.

      **This removes a lead #85 was following.** A LASERBEAM's `-1` at `+18` is not an unfilled object
      handle; the field is not an object handle at all.

      `gp+0x421C` is register-or-act, and LASERWALL proves it: the same flag, read at `0x8002E228`, sends it
      to a list when set and straight to `T_Damage` (mod 11) when clear. A port need not model it — raising
      the beams at zone load is when the console's registration pass raises them.

- [x] 90. **Effects were sorted on a different depth scale from the world, and lost every argument.**
      Found by wiring #89 and getting eleven beams queued, **970 faces emitted, and seven pixels different
      on screen.**

      `effect.c`'s `bucket_for` was `(depth / corners) >> 2` — the fixed shift the whole port used before
      the viewport's far distance was available. `world.c` and `modeldraw.c` had both moved to `far_z`
      scaling; the effects had not. Against the console's 217-bucket table (and a real viewport slice of 51)
      the shift saturates: a beam 700 units away becomes otz 175 and lands in bucket 41, while the wall at
      the same 700 units lands in 193. Bucket 0 is drawn first. **Every particle, glint and beam more than a
      room away was emitted correctly, sorted behind the walls, and painted over.**

      The fix is to share the world's mapping, and it is not a tuning choice: an effect and the wall behind
      it have to be measured against the same far distance or the sort between them means nothing.
      `tests/test_effect.c` now pins it by comparison rather than by constant — the bucket an effect at
      depth d lands in must be the bucket the world's own mapping gives d — and asserts that the shift this
      replaced buried a near effect behind its wall.
- [x] 91. **MISEVENT's namespace, located: it was never a Strings key.** 0 of 93 resolved against the
      Strings chunk, and that is because Strings is the wrong chunk.

      The exec at `0x8002BA1C` gathers item +4..+15 **byte by byte** into three words — an unaligned
      twelve-byte load, not three numbers — and hands them to `0x800419A0`, which searches with the
      twelve-byte compare at `0x8006DB10` and then does this:

          800419FC  lw   s4, 17480(gp)     ; pass 0: the executable's namespace
          80041A08  lw   s4, 12516(v0)     ; pass 1: [0x800B30E4], the level's
          80041A1C  jal  0x8006DB10        ; (key, list, stride 16)
          80041AD8  lw   v0, 12(s2)        ; the found record's +12...
          80041AE8  jalr v0                ; ...IS A HANDLER, and it is called

      So a namespace is a table of `name[12] + handler`, sixteen bytes a record, NUL-terminated — the
      UserFuncs binding table's shape with one pointer instead of two. **The executable carries one at
      `0x8009B680`, three records long, immediately before the UserFuncs table itself**: `Pump1On`,
      `Pump2On`, `CheckPumps`. That is WASTE3's coolant pumps, and it pairs exactly with the line #74
      found in that map's own Strings — `Find and activate both coolant pumps.`

      The second namespace is the map's, and no engine code ever stores a non-zero into `[0x800B30E4]`, so
      a module writes it. **The module carries the table as data**, and it can be recovered rather than
      executed, the way the group selector is (#87). LAB's is:

          801016D4  "Laser0\0\0\0\0\0\0"  80101628
          801016E4  "Laser1\0\0\0\0\0\0"  80101648
          801016F4  00000000                          <- the terminator

      **20 of the disc's 20 MISEVENT keys now resolve** — 3 in the executable's table, 17 in ten maps'
      own — with no false positive in QENDMIS5's thirty kilobytes of module text.

      Three anchors were tried and the two that failed are worth keeping, because each failed silently in
      a different direction:

      - *Any name-shaped record.* Walks straight into module text. A twelve-byte window inside
        `Initialise %` or `MDEC_out_sync` is printable and is followed by something small, so QENDMIS5
        reported twenty screens that do not exist.
      - *A NUL-padded record.* Fixes that and then misses any table whose names fill the field. BOSS1's
        begins `LaserButton0`, `LaserButton1`; BIGGUN's is `STOPPLATFORM`, `Destroy Grav`. All four are
        exactly twelve characters, and all four came back as script naming events that do not exist.
      - *The terminator.* The zero word is the engine's own marker — `0x8006DB10` stops there — so find
        one, walk backward while the record shape holds, take the maximal run. It anchors on structure
        rather than on how long a level designer's names happen to be.

      One more guard was needed and its first form was also wrong. A record may not begin inside a
      printable RUN, or BIGGUN's backward walk runs on into `TeleportDeat` and `royGlassZone` — windows
      cut out of longer strings. But "the preceding byte is not printable" is too blunt: three real tables
      sit immediately after a function epilogue, and `addiu sp, sp, 32` ends in `0x27`, an apostrophe.
      BASE0's `DOCRATES`, COMMAND's `Comp1` and JAIL3's `Bridge` all vanished on that. Three consecutive
      printable bytes is the line.

      **Where it stops is the handler.** It is MIPS in the module and this port does not run modules, so
      each live handler still has to be reconstructed. BASE0's `DOCRATES` now is: the named `CRATES`
      zero-speed LIFT1 supplies four runtime objects, slots 0/1 advance by `(16*dt)/8`, slots 2/3 by
      `(20*dt)/8`, and each wraps by -3500 once its translated box centre reaches -1044. The named `ALWAYS`
      record is queued once per actual simulation tick, which is how retail continuously calls it. Handlers
      not yet reconstructed remain named and counted rather than failing silently.

      The engine's own half either side of the handler is two lines and neither needs reproducing:
      `0x8006D2EC` parks the twelve-byte name in a global at `0x800DD950`, and `0x800435D0` spills three
      words and returns — a logger compiled out of the retail build.
- [x] 92. **QENDMIS is the MOVIE PLAYER, which answers #16 and unblocks #15.** The campaign's last map
      loaded as two quads, eight vertices and a black screen, and that was recorded as end-of-mission
      content this port fails to draw. **There is no content to draw.** A `QENDMIS<N>` map is a container
      for the movie player overlay, and its LevelBin says so outright:

          module+0x0310  "\Q2DATA\"          module+0x03E0  "MDEC_in_sync"
          module+0x031C  "MOVIES\"           module+0x03F0  "MDEC_out_sync"
          module+0x033C  "ring buffer"       module+0x0348  "vlc buffer 0"
          module+0x0368  "image buffer"      module+0x0358  "vlc buffer 1"

      with a table of 36-byte records from module+0xB8 — three twelve-byte fields, a screen name, a trace
      label and a FILENAME:

          +0x00B8  "Intro FMV"   "Do Intro\n"   "TAKE1BP.STX"
          +0x00DC  "Extro FMV"   "Do Extro\n"   "OUTRO1P.STX"

      and the disc carries exactly those, 15.1 MB and 19.5 MB, in `/Q2DATA/MOVIES`.

      **This answers #16**, which read: *"The executable contains no `.STX` / `MOVIES` / `STX` string at
      all, so both the player and its filename assembly live elsewhere."* Elsewhere is here. All five
      QENDMIS maps carry the identical module and no gameplay map carries any of it. #15 — the MDEC output
      depth — was blocked on #16 and is now blocked only on reading this module's code.

      A module+0x7C table of twelve-byte names sits above it: `EndMission 1`..`EndMission 5`, then
      `QLogos2`, `QLogos`, `Dummy`, `MagazineIntr`, `MagazineExtr`, `MPResults`. So the same module is the
      whole front end — logos, magazine demo bookends and the multiplayer results screen, whose text is
      also in there (`DM SCORES`, `%s TEAM SCORED %d`, `ALL PLAYERS PRESS` / `FIRE TO CONTINUE`).
      `ROGUEINP.STX` is on the disc and is named by neither the module nor the executable.

      **What the port does about it.** It cannot decode MDEC, so it stops pretending: `q2_levelbin_movies`
      recovers the table, and `q2_endmission_build_ot` draws a placard on the console's own panel naming
      the unit and what would play. A campaign run from BASE0 still walks twelve levels and five units and
      now *ends* on something rather than on a black field that reads as a crash.

      The placard does not guess. The disc has one outro and only the last unit reaches it, so units 1..4
      say the sequence is drawn by the map's module and name no file — a confident wrong answer on
      QENDMIS1 would be worse than a short true one.

- [x] 93. **`--fire-triggers` was disabled by a warning fix, in the same session that wrote the warning
      fix.** Worth recording because the failure mode is the interesting part.

      The gate read `if (c.fire_triggers && c.sim_ready && ...)`. `sim_ready` is `bool[Q2_MP_MAX_PLAYERS]`,
      so the test was an array decaying to a pointer: always true, and the build said so
      (`-Wpointer-bool-conversion`). Changing it to `c.sim_ready[0]` looks like the obvious fix and is
      wrong — `sim_ready[]` is filled only by the MULTIPLAYER spawn path and player 0 never enters it. The
      flag went from accidentally-always-on to silently-always-off, the campaign run stopped at BASE0, and
      nothing failed: no warning, no error, 27/27 tests still green.

      What the gate actually wants is the level's triggers to be loaded, which is the thing the loop under
      it walks. A whole-campaign run is the only check that would have caught it, and it is the check that
      did.
- [x] 94. **Every chunk the client read after a map change was read through a dangling pointer.**
      Found by sweeping all 49 maps for warnings and errors instead of reasoning about which might fail.

      `q2_common_file` holds `dat_archive ar` — whose directory is INLINE, `dat_chunk
      chunks[DAT_MAX_CHUNKS]` — and beside it `const dat_chunk *chunk[]`, an array of pointers **into that
      inline array**. So the type is not safely assignable, and the client's map change assigned it:

          q2_common_close(&c->common);
          c->common = common;          // `common` is a LOCAL

      Every `chunk[i]` in the destination still aimed at the source's storage, and the source died at the
      end of the block. From that point on the Events chunk, the Scene, Strings, LevelBin and everything
      else were read out of a dead stack frame that usually still held the right bytes.

      Usually. On BIGGUN, FRAGTOWE, MATRIX1 and MATRIX5 the frame was reused before the level's first
      `TELEPORT` ran, and the StartPos parse read a size of **2155905024** — 0x80808080. That is the only
      way this ever announced itself, and it announced itself as a format error in a chunk that verifies
      clean on all 49 maps.

      `q2_common_move` / `q2_zone_move` copy the archive and then **re-resolve the directory against the
      destination's own storage** — the same call `q2_common_open` makes, so there is one place that knows
      how a directory is built — and empty the source so it cannot double-close.

- [x] 95. **A whole-disc load sweep: 49 of 49 maps now load with no warning and no error.** It was 1 of 49.
      Nothing else in this entry is a bug fix; it is the log telling the truth about what it found, which
      is what made #94 visible in the first place.

      Four classes of noise, each of which was a correct disc fact reported as a fault:

      - **21 "cannot read ZONE`N`.DAT: not found" at ERROR.** Nothing on the disc says how many zones a map
        has, so the client counts by probing until one is absent. The absence IS the answer. A zone that
        exists and will not read is still an error.
      - **28 "carries no `qk_menu.lbm` — the status bar will be blank".** The icon sheet is chosen by
        session, and **an arena carries only the multiplayer sheets** because an arena cannot be played in
        single player — MATRIX1 opened with `--dm` resolves its sheet immediately. The front-end maps carry
        no sheet at all because they draw no status bar. Warning on all 28 buried the one case that would
        have been a fault.
      - **4 "no music playlist".** Those four directories — FRAGTOWE, QSTARTUP, QINTER, QMAGINTR — are not
        in the LEVEL TABLE at all, so they have no playlist to have. The message blamed the music table for
        the level table's silence.
      - **1 "carries no menu font".** QFMV is the movie stub: 46 KB, no `frontend.lbm`, no `chars.lbm`, no
        icon sheet. Warning that it has no font is warning that a film has no subtitles.

      The general lesson is the one #94 depended on: a log that warns about normal conditions is a log
      nobody reads, and the one real fault on the disc was sitting in the middle of 54 lines of correct
      behaviour being reported as wrong.
- [x] 96. **The view weapon left the hand whenever the player looked up or down — and #46 walked past it
      three times because it was checking the wrong kind of thing.**

      #46 read every operand in `q2_vw_place` against the executable and every one agreed: the translation
      is 140 on every key, the interpolation is the disc's, the rotation order is `RotMatrix`'s Ry·Rx, the
      eye base is `286 − viewOffset`. Three passes, all correct, and the weapon was still wrong.

      The defect is not in an operand. It is in the IDENTITY the whole arrangement rests on, and nothing
      asserted it. Follow a part origin through `modeldraw.c` — with `local` zero at the grip,
      `inst.origin = feet + R_place·t`, and `cam.pos` the eye, which is the same `feet − view_offset`
      `q2_vw_place` computes, deliberately:

          camera_space = view · (inst.origin − cam.pos) = view · R_place · t

      so the weapon lands where the clip authored it — at `t`, in view space — **only if `view · R_place`
      is the identity**. The two matrices came from different builders fed differently-signed angles:
      `q2_rotation_view(yaw, pitch, roll)` against `q2_rotation_euler(−pitch, yaw, roll)`. They agree on
      yaw. They do not agree on pitch. Measured:

          yaw    0  pitch    0  roll   0     identity
          yaw 1024  pitch    0  roll   0     identity
          yaw  700  pitch  300  roll   0     off by 3260/4096
          yaw  700  pitch  300  roll  90     off by 3532/4096
          yaw 3000  pitch −200  roll −90     off by 1878/4096

      3260/4096 is not rounding. It is a rotation, and it meant the grip was placed by one basis and read
      back by another the moment the player stopped looking level. On screen: pitched down, the blaster
      floated up beside the crosshair with no arm under it; pitched up, it swung to the top-right corner.
      **At pitch zero nothing moved, which is exactly why every still-frame comparison #46 made kept
      agreeing with the code.**

      The fix is to build the placement matrix with the CAMERA'S OWN function and apply its transpose. A
      rotation matrix's transpose is its inverse, so `view · R_place == I` becomes true **by
      construction, for any angles** rather than by two builders happening to agree. That is the identical
      argument `q2_vw_build_ot` already makes for the ROTATION half — "taking the camera directly makes
      `camera · (camera^T · clip) == clip` true by construction" — and it had simply never been applied to
      the translation.

      **This does not settle #46's original measurement.** That was a still frame at rest, and at rest
      nothing here moves. Whether the remaining horizontal offset is real or was measured against a
      pitched capture is now a much easier question, because the pitched case is no longer wrong.

      Two tests, and the second is a repeat offence. `test_camera_undoes_the_placement` asserts the
      identity across ten cameras — the check that did not exist. And `test_roll_reaches_the_offset` had
      to be rewritten: it recovered the offset with the same `q2_rotation_euler` the emitter used, so the
      test and the emitter agreed with each other and neither agreed with the camera. **That is the second
      test this session written from the code under test rather than from what the code owes its caller**
      — the briefing panel's was the first — and both pinned the defect instead of the requirement.
- [x] 97. **The animation path now poses every named move by name — it was doing so for 40% of them.**
      #63 closed the chain (name -> block D -> `base` -> `position = base + 30*(f − first)` -> walk) and
      wired it, and reported the split honestly: `BASE2 1745 poses by name, 2655 by the fallback`. The
      fallback is `q2_model_anim_by_length`, which matches a clip by LENGTH — something the disc never
      does. So most poses were still being chosen by a mechanism that does not exist on the console, and
      the entry left it there.

      Splitting the one failure counter into two says where it goes, and the answer was not what the
      remaining-work note assumed:

          BASE1   431 by name, 369 named but no position   -- 0 of those names missing from block D
          BASE2   322 by name, 678 named but no position   -- 0 missing
          JAIL4   847 by name, 753 named but no position   -- 0 missing

      **Every name resolved.** The pairing was complete. What failed was the walk: the position landed past
      the end of the timeline. Traced, the case is exact — the Enforcer's `Duck` is a **30-frame AI move
      whose animation begins at model frame 1287 of a 1302-frame timeline.** Five AI frames of animation,
      twenty-five frames of nothing after it, and at `into = 5` the position is frame 1302: one past the
      last.

      That is #63's own finding arriving in the renderer. An AI move's length and its animation's extent
      are different quantities authored independently, so a move CAN outlast its animation — and when it
      is the last move on the timeline there is nothing after it to run on into. The engine does not care:
      its loop (`0x8006B924`, with `0x80070188` as the advance) has **no end-of-chain test at all** and
      would read past the block, because on the console the move's end callback has fired by then and the
      creature is in a different move.

      `q2_model_anim_at_held` holds the last frame instead, and reports that it did. It is a stated
      divergence and the smaller of the two available lies — the alternative was falling back to length
      matching, which is not a divergence but a different mechanism. `q2_model_anim_at` keeps the engine's
      exact loop, and `tests/test_model.c` pins the two apart against a synthetic three-clip chain: inside
      the timeline both agree and neither reports a hold; one past, the engine's walk fails and the hold
      lands on the last clip's LAST frame; far past, the same frame, because a hold must not drift.

      **And a garbage move name, found by the same counter.** With the walk fixed, JAIL4 still showed 177
      names missing from block D — and the name was `@.B$L`. `name_slot_ok` required a printable run of
      three or more and never required it to be TERMINATED, so a five-character window cut out of a
      module's code, followed by two halfwords that happened to satisfy `last >= first`, was accepted as a
      move name. Two guards fix it, and they are the two the LevelBin mission-event scan needed for exactly
      the same reason (#91): the field must be NUL-terminated with its padding checked, and a record may
      not BEGIN inside a printable run.

      Checked against the thing that would have caught over-tightening: **108 of 115 moves carry the
      module's own name both before and after**, so no real name was lost and one invented one is gone.

          JAIL4   1570 by name, 0 missed, 30 unnamed      (was 1393 / 177 / 30)
          BASE2    994 by name, 0 missed,  6 unnamed
          POWER1   856 by name, 0 missed, 144 unnamed

      The `unnamed` remainder is the honest fallback case and the only one left: a decoded move the module
      never names has nothing else to go on.
- [x] 98. **The creature sound table is not a table — the module REGISTERS each sound by name, and the
      play site names the BSS word it registered into.** #61 tried four ways to pin where a creature's
      sound list starts, reverted three of them, and its third attempt concluded that the model of the
      problem was wrong. It was.

      `q2_creature_sound_names` finds a RUN of 12-byte name slots and calls that the table. The run is a
      string POOL. The Berserk's proves it: the "table" comes back as
      `Attack1 Attack2 Attack3 ber_pain2 inf_pain1 ber_deth2 inf_deth2 …` — three of its own MOVE names,
      then a mix of its sounds and the INFANTRY's. Slot index was being used as the sound number, so a
      pool read as a table misnumbers everything after the first wrong entry.

      What the module actually does, at init:

          801008D4  addiu t0, v1, 420      ; t0 = the name, module+0x1A4
          801008D8  lbu   v0, 1(t0)        ; packed into a0..a3 a byte at a time,
          801008DC  lbu   v1, 420(v1)      ;   because it is unaligned
          801008B8  lw    v0, 36(s2)       ; import slot 9, the sound loader
          801008C0  jalr  v0
          801008CC  sw    v0, 5976(v1)     ; the HANDLE, into module+0x1758

      and the code that plays a sound names that BSS word — which is exactly what the decoder already
      printed at every site as `sound(8010175C)`, and exactly what #61 found to be zero in the image. Zero
      because it is filled at load. **The name that fills it is in the instruction stream.**

      `q2_creature_sound_bindings` decodes those registrations into address/name pairs. It is the same
      by-value 12-byte decode the LevelBin group selector needs, tracking each register's most recent
      `lui` so the name pointer is computed rather than guessed at. All seven creatures decode, the two
      that returned nothing among them:

          Soldier  12   Tankcomm  8   Insane 3   Arachner 8
          Gunner   11   Infantry 12   Berserk  10

      And the duplicates explain the pool. `80101750=ber_pain2` and `80101750=inf_pain1` register to the
      SAME word; so do `801032C4=wep_shotgr1` and `801032C4=wep_sshotr1` for the Soldier. **One module
      serves several classes and the class byte picks which registration runs** — which is why the
      Berserk's module carries `inf_*` strings at all, and why no run-start could ever have been right.

      **The consumer was wrong too, and in a way the counter hid.** `cre_actions.c` passes
      `q2_cre_action.addr` — the module address — straight to the sound hook, and `main.c` handed it to a
      function that INDEXES a name list. Every decoded creature was asking for name 0x80101758 of eight.

      **And the counter that would have said so was measuring the audio device.** `%u sounds (%u not in
      bank)` incremented when `client_play_sound` returned false, and that function returns false when
      `c->audio` is NULL — which is every headless run. So `7 sounds (7 not in bank)` on JAIL4 meant "this
      run had no speakers", while JAIL4 carries `sol_idle1`, `sol_sght1`, `sol_pain1` and `sol_deth2` and
      found every one. Counting on bank residency instead:

          JAIL4   7 sounds, 0 not in bank      BASE1  4 sounds, 0 not in bank
          WASTE4  6 sounds, 4 not in bank      POWER1 7 sounds, 7 not in bank

      **The remainder is the disc, not the port.** The names those two ask for and do not get are
      `ber_idle1` and `gun_idle1`, and a sweep of every `SNDVRAM.DAT` on the disc says why:

          ara_idle1  ara_srch1  ber_idle1  ber_srch1  tnk_idle1   -- NOWHERE on the disc
          gun_idle1                                               -- on 13 maps, but not POWER1

      A module registers more sounds than any bank carries. The console's loader returns a null handle for
      those and playing one does nothing, so the silence is retail behaviour and matching it is correct.
      Recorded because it is exactly the shape of a gap and is not one.
- [x] 99. **The seven unnamed moves cannot be resolved by position, and the reason closes the question
      rather than leaving it open.** #97 left `POWER1 856 by name, 0 missed, 144 unnamed` and the unnamed
      remainder looked like the next thing to chase. It is not chaseable, and measuring why is the answer.

      Disc-wide there are **exactly seven unnamed moves and exactly one per creature**:

          Soldier 97-98   Tankcomm 30-33   Insane 0-39   Arachner 25-33
          Berserk 36-41   Gunner 94-101    Infantry 74-85

      A move is named by a 20-byte `{char[16], u16 first, u16 last}` record matched on frame range, so an
      unnamed move is one whose range the module's own table has no record for. One per module is a
      pattern rather than seven accidents, but it does not make the name recoverable.

      **The idea worth testing was position.** Block D's records tile a model's timeline contiguously
      (`test_moves_tile_without_gaps`), and the AI ranges tile too — so if the two orders agreed, an
      unnamed move's record would be determined by its named neighbours' and nothing would need guessing.

      They do not agree, and not marginally. Crossing the Soldier's 27 named moves against BASE1 model 15's
      block D, in AI-frame order:

          ai    0  Run            block-D  2538
          ai    1  Fire 1 Ready   block-D  2292
          ai   45  Attack4        block-D   498
          ai   50  Walk           block-D  1950
          ai  308  Death1         block-D     0
          ai  465  Death5         block-D   852

      `Run` is the first move the AI has and the *last* stretch of the model's timeline; `Death1` is late
      in the AI's ordering and sits at block-D zero. **The two orderings are independent**, which is #63's
      own conclusion — durations and extents are different quantities, authored separately — arriving in
      one more place.

      So there is nothing between the neighbours to interpolate, and matching a clip by LENGTH is the only
      mechanism left for those seven. That is what `q2_model_anim_by_length` is for and what the code
      already says it is for; what changes here is that it is now known to be needed for **one move per
      creature and nothing else**, rather than being an open-ended fallback of unknown size.
- [x] 100. **The front end's pages are now READ out of QFRONT's module, not just transcribed from a
      capture — and the port's own checker was reporting 26 failures for looking in the wrong file.**

      #44 established that the front end is `QFRONT`'s `LevelBin` (118,216 bytes against 13,008 of
      `LevelRel`), that its pages are static 24-byte record arrays in the executable's own layout
      (`{ char *text; s16 x; s16 y; void (*action)(void); … }`), and transcribed a dozen of them by hand
      with their coordinates. What it did not do was make that transcription checkable.

      `q2_levelbin_menu_pages` reads them. The anchor is the TEXT POINTER — it must land on a printable
      NUL-terminated string inside this module — and then x must be **256**, which every front-end row is
      centred at, and y on screen. Two independent things have to hold before a record is believed, and a
      run of two or more makes a page. **45 pages, 186 rows**, and they agree with #44's hand
      transcription exactly:

          +0x0EC3C  START            256, 151      +0x0ED44  PLAYER OPTIONS  256,  85
          +0x0EC54  OPTIONS          256, 177      +0x0ED5C  SOUND OPTIONS   256, 111
          +0x0EC84  SINGLE PLAYER    256, 111      +0x0ED74  VIDEO OPTIONS   256, 137
          +0x0EC9C  MULTI PLAYER     256, 137      +0x0ED8C  VIEW CREDITS    256, 163

      A **null action had to be legal** or nothing would parse: the deathmatch setup page's rows have
      bytes +8 onward all zero, because its values live in the text and the module rewrites them in place.
      So the terminator cannot be a null action, and the run's end is where the record shape stops
      holding.

      The reader also turned up pages the capture never showed, because they are transient — `STARTING` /
      `GAME` for the attract loop's hand-off, `USE DIRECTIONAL BUTTONS` / `TO ADJUST DISPLAY` for the
      screen-position adjuster, and a controller-port prompt.

      **And the checker.** `cmd_menu` verifies every transcribed page against the executable's segment,
      which is right for the 37 in-game pages and wrong for the front end's — those live at 0x8010xxxx in
      a module. Every one of them produced `record escapes the segment` per item and then
      `8010EC3C holds 0 records, the port transcribes 2`: **26 complaints that said nothing except that
      the reader was pointed at the wrong file.** They are now recognised as module pages and sent to
      `menu front`, and the run reads `37 pages checked, 0 mismatches`.

      That noise mattered for the same reason #95's did. A checker that cries wolf on correct data is a
      checker nobody reads, and this one had been reporting 26 failures for as long as the front end's
      tables have been in the port.
- [x] 101. **The scoreboard's static furniture is read, its six titles recovered, and the QENDMIS maps
      turn out to be one file five times over.** #46 read the capture and concluded the missing piece was
      "the same run-time item construction as #44". #100 built the reader #44 needed, and it applies here
      unchanged — the engine's page walker takes a module's record and the executable's without knowing
      the difference, so one decoder serves both.

      `QMRESULT`'s module carries **exactly one page, of exactly the two rows the capture shows**, with
      coordinates the capture could not give:

          module+0x051F0  ALL PLAYERS PRESS   256, 180
          module+0x05208  FIRE TO CONTINUE    256, 200

      Centred at 256 like every other page in this family, on a **20-pixel pitch** — tighter than the front
      end's 26 and looser than its five-row 22. And one page is the finding, not a shortfall: the
      per-player rows are NOT menu records. They are built from the match state, which is why nothing
      static describes them and why #46's guess that they were widgets was the right thing to check and
      the wrong answer.

      **The six scoreboard titles are in the same pool**, in mode order, and the capture could only ever
      show one of them:

          DM SCORES · TEAM DM SCORES · CTF SCORES · TAG SCORES
          TEAM TAG SCORES · VERSUS SCORES

      six titles for the six modes `QMULTI.C` implements of which three are selectable (#0), alongside
      `%s TEAM SCORED %d` and the team colour names `BLUE`, `PURPLE`, `GREEN`. #44 found five deathmatch
      RULE paragraphs where the capture showed three; this is the same three-of-six split seen from the
      results end.

      **And the front-end family shares its module.** Hashing every `COMMON.DAT` in it:

          QENDMIS1..QENDMIS5   e9b11b33d7f0   88,804 bytes — ONE FILE, five directories
          QSTARTUP, QINTER     80a2498b432e   53,544 bytes — likewise
          QMRESULT  101,664   QLOGOS  102,464   QFMV  46,624   QFRONT  235,340

      That settles what #92 could only infer from the strings matching. `EndMission 1`..`EndMission 5` are
      five entries in a level table pointing at five directories holding **the same bytes**; the digit
      patched into the name at `0x8001900C` selects a screen inside one shared module, not a different
      level. It also explains why the movie table is present in all five and identical in all five.
- [x] 102. **The screen fade was CUT. The search space is now exhausted, not merely unsearched.**
      #39 established that `gp+16660` (`0x800B2714`, set to 255) and `gp+16676` (`0x800B2724`, set to −16)
      have exactly one writer each and **no reader anywhere in the executable** — not gp-relative, not
      through a materialised base, and neither address as a data word. It left two possibilities open and
      named the capability that would decide between them: *"Either the consumer is in an overlay (the
      movie player, #16) or a relocatable module (#6) … look for the readers once a relocated module can be
      disassembled."*

      Both of those are now reachable. The movie player is a `LevelBin` module (#92), and modules are
      decoded routinely. Scanning **every module on the disc** — 164 `COMMON.DAT` and `ZONE*.DAT` files —
      for any `lui 0x800B` paired with a load or a store at either low half:

          no reference of either kind, in any module on the disc

      And a module cannot be handed the address either: scanning all **213** files for the raw words
      `800B2714` / `800B2724` as data finds **zero**.

      Executable: one writer, no reader. Every module: no reference. Every file: not passed as data. That
      is the whole space. **The fade was cut and the two writes are vestigial**, which is what #39
      suspected and could not show. The port implements no fade, and now that is a finding rather than a
      default.

- [x] 103. **The view weapon's two angle triples, attributed: `player+230` is the AIM and `0x80038260`
      returns the KICK.** #41 recorded the ambiguity honestly — "nothing at the call site distinguishes
      them" — and noted that if the attribution went the other way nothing in the port would change. It
      does not go the other way, and the two are not symmetric at all once the second one is read.

      The call site takes the first straight out of the entity:

          8004F40C  lhu  v0, 230(s6)      ; the entity's own field, read and used
          8004F410  lhu  v1, 32(sp)       ; the triple 0x80038260 returned
          8004F418  addu v0, v0, v1
          8004F41C  subu v0, zero, v0     ; x negated at the sum

      `0x80038260` is not a field read. It is three near-identical blocks, each of which loads a DEADLINE
      (`lw v1, 32(s3)` / `36(s3)` / `40(s3)`), subtracts the current time from `0x800B2BAC`, drops out on
      `bltz` if it has passed, and otherwise scales a stored angle pair by the remaining fraction and
      **accumulates into the same output pair** — so the three sum. Three independent contributions, each
      decaying over its own period, added together: that is a kick and nothing else is.

      The periods are in the reciprocal-multiply constants, and they are the three the port already
      names:

          0x88888889  ->  / 30     the firing kick
          0x1B4E81B5  ->  / 150    the damage kick
          0xB60B60B7  ->  / 90     the landing kick

      So `main.c`'s standing comment — "0x80038260 composes three decaying kicks — firing over 30 ticks,
      damage over 150, landing over 90" — was right, and it is what settles #41: a field with no decay is
      the aim, and the thing with three decay periods is the kick. Nothing in the port changes, which #41
      predicted; what changes is that the ambiguity is closed rather than carried.
- [x] 104. **The engine does not loop an XA track — it FADES it and moves on, and the duration is the
      moment it moves.** #14 asked the question and said exactly what was missing: *"the consuming code was
      not disassembled."* It is four sites, and together they answer the question, explain the entry that
      measures 1.0 s short, and turn up a fade nobody had found.

      **The two globals, and what fills them.** `0x80071898` reads the music table's `tenths`;
      `0x800718C0` multiplies it by five into `0x800B2710`; `0x800718C8` copies the same value to
      `0x800B2708`. Tenths x 5 is tenths-to-50-Hz-ticks, and `0x80071878` supplies **300 tenths — 30.0
      seconds — when an entry names no duration**, which is #14's "30.0 s fallback" exactly. **A is what
      remains; B is the total.**

      **What reads them.** `0x80071954` and `0x80071980`, in the same handful of instructions:

          80071954  lw   a0, 0x800B2710     ; remaining
          8007195C  slti v0, a0, 64         ; ...under 64 ticks?
          80071964  mult v1, a0             ; volume * remaining
          80071970  sra  v1, v0, 6          ;              / 64
          80071980  lw   v0, 0x800B2708     ; total
          80071988  subu a0, v0, a0         ; elapsed
          8007198C  slti v0, a0, 64         ; ...under 64 ticks?

      A track **fades in over its first 64 ticks and out over its last 64** — 1.28 seconds each way at
      50 Hz. Nothing had found this; #39 spent a whole entry looking for a fade and this is not the one it
      was looking for, but it is a real one.

      **And what happens at zero.** `0x80071A58` tests the countdown, and when it has run out:

          80071A68  lw    v0, 1536(gp)      ; the playlist cursor
          80071A70  addiu v1, v0, 1         ; advanced
          80071A78  lb    v0, 1(v0)         ; the next entry
          80071A80  bne   v0, zero, ...     ; zero is the end of the list
          80071A88  jal   0x80071B6C        ;   ...which restarts it

      So the engine plays each track for its TABLE duration and then moves to the next, restarting the
      playlist when it runs off the end. **It does not loop a track.** And the entry that is 1.0 s longer
      than its table value is not an error, which is what #14 suspected: the last second is simply never
      heard, because the fade-out has finished and the cursor has moved.

      **Wired.** The port advanced only on END OF STREAM, so a track that outlasts its duration played to
      its own end and the fade did not exist at all. It now runs the countdown on the console's 50 Hz,
      advances on zero, keeps the end-of-stream path for a stream that runs out early, and applies both
      ramps. The constants are named — `Q2_MUSIC_FALLBACK_TENTHS` 300 and `Q2_MUSIC_FADE_TICKS` 64 — rather
      than left inside the expressions, because each is a separate reading of the disassembly and getting
      either wrong is a silent difference in how the music behaves.
- [x] 105. **Reverb is OFF, and the sample data proves it without needing the code.** #17 suspected as much
      from a "240-byte margin" and left it there. Measuring every map's SNDVRAM against SPU RAM settles it,
      and the margin is even tighter than the entry remembered.

      Summing the `VAGp` body sizes in each map's bank, against the SPU's 524,288 bytes:

          FRAGTOWE  60 samples   522,000 bytes    2,288 free
          WASTE3    82 samples   512,400 bytes   11,888 free
          JAIL3     77 samples   512,192 bytes   12,096 free
          POWER2    79 samples   511,344 bytes   12,944 free

      And libspu's reverb work areas, smallest first:

          STUDIO_A  8,000 · ROOM 9,920 · STUDIO_B 18,496 · HALL 44,512 · SPACE 63,168

      **The smallest one libspu offers does not fit in the tightest map's free space**, and FRAGTOWE's
      2,288 bytes are short of it by a factor of three and a half. The work area is a single global
      allocation rather than something re-sized per level, so it has to fit the worst map — and it cannot.
      Reverb is disabled.

      The port implements no reverb, and this is what turns that from a default into a finding.

      **Why the register scan found nothing**, recorded so it is not repeated: sweeping the executable for
      any `lui 0x1F80` paired with a load or store in `0x1F801D80..0x1F801DBF` returns **no site at all**.
      The game reaches the SPU only through libspu, which keeps its own attribute struct and flushes it, so
      `SPUCNT`'s reverb-enable bit is never written by an instruction this port can point at. That is why
      the answer had to come from the data rather than the code.

      One loose end, stated rather than smoothed over: FRAGTOWE's 522,000 bytes plus libspu's own 4,112-byte
      reserved area at the bottom of SPU RAM comes to 1,824 bytes MORE than the SPU has. So either not
      every sample in that bank is resident at once, or the reserve is smaller on this build. It does not
      change the conclusion — every reading of it leaves far less room than any reverb mode needs — but it
      means the exact SPU RAM base is still unread, which was the other half of #17's title.
- [x] 106. **The scoreboard's prompt was drifting with the player count, because it was being treated as a
      score.** #101 read QMRESULT's static page and found the two rows it holds — `ALL PLAYERS PRESS` at
      256,180 and `FIRE TO CONTINUE` at 256,200 — and that is the whole of what the module holds
      statically. The port had the right TEXT and the wrong idea of what it was.

      `q2_mp_scoreboard` appended both to its line list, and the client drew every line stacked 16 pixels
      apart from a fixed start. So the prompt sat two rows further down with four players than with two:
      it moved because the scores above it moved, which fixed furniture does not do.

      They are out of the line list now and drawn at the module's own coordinates, and the score rows grow
      between the title and them. `tests/test_multiplayer.c` pins the split by COUNT — a deathmatch
      scoreboard is a title and one row per player and nothing else — because a caller that draws every
      line it is handed would otherwise put them back by accident, which is exactly how they got there.

      What is NOT reconstructed, and is a real remainder: the capture also shows a per-row bar in each
      player's own colour, a `READY` marker on a row whose player has pressed fire, and the backdrop scene
      QMRESULT's `ModelNames` describe. The rows are text on the overlay here.
- [x] 107. **The movies: the demuxer is exact, the decoder is most of the way, and what is missing is five
      Huffman groups — named, counted, and bounded.**

      #92 established that `QENDMIS`'s module is the movie player and that the campaign ends on a 19.5 MB
      `.STX`. FORMATS.md §6 had the container settled — verified across all 32,442 sectors — and no decoder
      had ever been written, so the ending was a placard. This is the decoder, as far as it goes, and the
      part that does not go is stated rather than implied.

      **The demuxer is exact.** `q2_stx_frame_next` walks the 8-sector interleave, skips the audio slot and
      the nulled tail, and reassembles each frame from its 5 or 6 chunks with every header cross-checked
      against the frame it claims to belong to. Frame counts come out **1283 / 1559 / 2459** — the three
      numbers §6 recorded from an independent pass, which is the check.

      **The bitstream decoder's structure is right.** 243 of OUTRO1P's 1559 frames decode to exactly 1440
      blocks — 20x12 macroblocks of six — consuming exactly **12 bits per block**, which is a 10-bit DC and
      a 2-bit EOB and nothing else. Those are its fade frames, and they come out flat, which is what a
      DC-only frame is. So the sector walk, the block order, the DC width, the EOB code and the geometry
      are all correct; a frame with no AC coefficients decodes end to end.

      **What is missing is the tail of Table B.14, and the histogram names it.** Every lookahead the AC
      table cannot match is bucketed by its run of LEADING ZEROS, because that run is how B.14 is
      organised. Across OUTRO1P:

          unmatched: 1316   [7 zeros] 845  [8] 297  [9] 73  [10] 58  [11] 43

      Five groups, nothing outside them, no scatter. The codes with 2 to 10 bits are in and correct; the
      12-, 13-, 14-, 15- and 16-bit groups are not transcribed. That is a bounded piece of work — five rows
      of a published table — rather than an unknown, and until it is done **the movies do not play**.

      **Two errors this pass caught mechanically, worth keeping.** A first attempt put an 8-bit group at
      `00101xxx`, which is a prefix collision with the 5-bit `00101`; and put the 10-bit group under
      `000001`, which is the ESCAPE, making every one of them unreachable while the escape swallowed bits
      belonging to a coefficient. Checking every pair of codes for one being a prefix of another found both
      in seconds, and neither would have been obvious from the output — a prefix collision produces a
      decoder that works on most blocks and desynchronises on the rest, which is exactly what "6 of 1283
      frames decoded" looked like.

      The IDCT here is a float separable one and is deliberately not the MDEC's fixed-point transform, so
      it will differ in the last bit or two. That is stated at the function rather than left to be
      discovered.
- [x] 108. **The movie decoder's Huffman lengths, DERIVED from the disc — unmatched codes 1951 -> 110, and
      the remaining fault is now named as a different one.** #107 left five groups missing and said a
      published table was needed. It was not. The disc has the codes in it.

      **The method.** Bail at the first unmatched code, so each failed frame contributes exactly one
      sample; bucket every sample by its run of LEADING ZEROS, which is how Table B.14 is organised; and
      for each bucket collect the distinct TAILS after the leading `1`. A Huffman code is prefix-free, so a
      group whose tails fill exactly four bits with the fifth taking both values is a four-bit code plus a
      sign, and its length falls out as `zeros + 1 + 4`. That is a measurement, not a recollection.

      Applied twice, because fixing the first two groups revealed the next three:

          7 zeros, 4-bit tail  -> 12 bits, 16 codes
          8 zeros, 3-bit tail  -> 12 bits,  8 codes
          9 zeros, 4-bit tail  -> 14 bits, 16 codes
         10 zeros, 4-bit tail  -> 15 bits, 16 codes
         11 zeros, 4-bit tail  -> 16 bits, 16 codes

      105 codes, and a pairwise check confirms not one is a prefix of another. **Unmatched lookaheads fell
      from 1,951 to 110 on OUTRO1P**, and the same on the other two films.

      **An analysis bug nearly ended this, and it is the part worth keeping.** The tail extractor shifted by
      `17 - (lz + 1) - 8`, which goes NEGATIVE once lz reaches 8 — so the 9-, 10- and 11-zero buckets each
      reported ONE constant tail at every width. That reads exactly like padding after a lost sync, and the
      conclusion drawn from it was that those buckets were not real groups. They were three real groups,
      and the evidence saying otherwise was undefined behaviour in the instrument. What caught it was
      printing the actual failing lookahead instead of trusting the summary of it.

      **What is left is a DIFFERENT fault, and the counters now say so.** Splitting why a block gives up:

          TAKE1BP    98 unmatched code, 1176 run overran 63, 0 out of bits
          OUTRO1P   110 unmatched code, 2475 run overran 63, 0 out of bits
          ROGUEINP  122 unmatched code, 4886 run overran 63, 2 out of bits

      A run that walks the coefficient index past 63 is a wrong RUN VALUE, not a wrong length —
      synchronisation only ever depended on the lengths, which is why the table was written with its
      run/level assignments flagged as not derived. Those assignments are the remaining work and they
      cannot come from sync: a wrong one produces a frame that decodes to a wrong picture, and the disc
      cannot say which picture is right.

      **So the movies still do not play**, and the reason has changed from "five code groups are missing"
      to "the run/level column is wrong", which is a smaller and better-defined thing than it was.
- [x] 109. **The movie decoder's remaining fault is NOT the run column, and the instrument that said it was
      is measuring the victim.** #108 split the failures into "unmatched code" and "run overran 63" and
      concluded the second was a wrong run/level column. Splitting that number by CODE LENGTH says
      otherwise:

          overran by code length:  0-bit 141  2-bit 178  3-bit 126  4-bit 115  5-bit 142
                                   6-bit 192  7-bit  89  8-bit 109  10-bit 37  12-bit 129
                                   14-bit 29  16-bit 12    escape 141, largest run 63

      **The 2-bit code carries run 0.** It cannot overrun unless the coefficient index is ALREADY 63 when
      it arrives — and it is, 178 times. So the overrun is not the code that reports it; one wrong jump
      earlier leaves the index high and every code after it reports the failure. The histogram identifies
      whichever code was next, which is exactly the shape of a measurement that looks conclusive and names
      the wrong thing.

      Two facts survive and are worth carrying:

      - **`largest run 63` and 141 escapes.** The escape's run field is six raw bits, so 63 is its maximum
        and hitting the maximum repeatedly is what a MISREAD FIELD looks like — either its width, its
        position, or the level width after it. That is the first thing to check.
      - Setting every long code's run to zero moved the total by 60 frames out of 1559, which is far too
        little for the long codes to be the cause. Whatever is wrong is in the common path.

      So the state is: the container is exact, the geometry, DC width and EOB are confirmed by 248 frames
      that decode end to end, the code LENGTHS are derived from the disc (#108), and something in the
      common AC path — most likely the escape's field layout — puts the coefficient index somewhere it
      should not be. **The movies do not play.**
- [x] 110. **The escape is ruled out by a sweep, AC decoding is proven to work, and the fault is the
      run/level column after all.** #109 doubted that diagnosis on the strength of an overrun histogram
      that turned out to be naming victims. Three measurements settle it.

      **The escape layout makes no difference at all.** Nothing in the bitstream announces how wide the
      escape's run and level fields are, so rather than assert a pair, `movie sweep` tries eighteen —
      run 5/6/7 against level 8/9/10/11/12/16 — and scores each by frames decoded:

          every one of the eighteen scores exactly 248 of 1559

      A parameter that changes nothing is not the cause. It also means the successful frames never reach
      an escape, so the sweep cannot pin the layout either; it is ruled out, not solved.

      **EOB really can be the first AC code.** The hypothesis that `10` at the first coefficient is
      (0, -1) rather than end-of-block — which would explain blocks terminating early — was tested and is
      WRONG: it takes the count from 248 to **zero**. BS v2 is not MPEG-1 in that respect.

      **And AC decoding works.** Counting the completed frames that consumed more than twelve bits per
      block — i.e. that carried real AC coefficients rather than a DC and a terminator:

          TAKE1BP 3 of 9      OUTRO1P 9 of 248      ROGUEINP 18 of 34

      up to 42,824 bits in a single frame. So the AC path is not fundamentally broken. Thirty frames
      decode end to end with genuine coefficient data.

      **What they look like is the answer.** Dumped, an AC-carrying frame is not a picture: it is bands of
      magenta, green and cyan blocks over a flat grey field — the signature of chroma coefficients taking
      large wrong values. The frame CONSUMES the right number of bits and produces the wrong numbers,
      which is precisely and only a run/level fault.

      So #109's correction was itself too strong. The histogram was naming victims — that part stands —
      but the conclusion it displaced was right: **the lengths are derived and correct, and the run/level
      column is wrong.** Completing it needs the actual Table B.14 assignments; they cannot be derived from
      synchronisation, because every wrong assignment that keeps the length produces a frame that decodes
      to a different wrong picture, and the disc cannot say which is right.

      **The movies do not play.**
- [x] 111. **"The disc cannot say which picture is right" was wrong, and building the oracle showed why the
      decoder cannot be finished this way.** #110 closed on that sentence. It is false in principle — a
      frame of real video is SMOOTH and a frame of wrong coefficients is not, so mean absolute difference
      between adjacent pixels judges a candidate table without any reference at all. `movie` now reports it.

      The oracle works and its first reading is the useful one:

          TAKE1BP  roughness 1.21 over 3 AC frames
          OUTRO1P  roughness 0.18 over 9
          ROGUEINP roughness 0.48 over 18

      **Those are far too LOW.** Real video at 320x192 runs to several units; 0.18 is a frame that is
      almost uniform. So the frames that complete are not noisy — they are nearly flat, and the AC
      coefficients they decode are barely reaching the picture.

      **And that is the trap.** The only frames the decoder completes are the near-flat ones; every frame
      with real content still fails. So the oracle can only ever score the least informative frames in the
      film, and a table tuned to make THOSE smooth is being tuned on the frames that carry almost nothing.
      The measurement is sound and the sample it can reach is biased, which is a different problem from
      the one #110 named and a worse one.

      One consequence, recorded so it is not retried blindly: removing the `/ 8` from the AC
      dequantisation — arithmetically an eight-fold change in every AC amplitude — moves the roughness by
      about **two**. That does not add up, and it is more evidence that the completing frames carry almost
      no AC energy rather than evidence about the formula. The change is NOT applied; replacing a sourced
      formula with an unexplained one is worse than leaving it.

      So the position is unchanged and better understood: lengths derived, escape not reached, run/level
      column wrong, and **no self-contained way to fix it** (and see #112, which withdraws "correct" from
      the lengths and explains why every metric here was satisfied by a wrong table) — the frames that would judge a candidate are exactly the frames that do not decode.
      **The movies do not play.**
- [x] 112. **A correction to my own evidence: "frames decoded exactly" does NOT validate the AC table, and
      the length derivation is therefore unverified.**

      #108 derived the code lengths from the disc and #110 called them "derived and correct". That second
      word is not supported, and the picture is what shows it.

      An AC-carrying frame that DECODES — right block count, right bit consumption — looks like this: the
      first few macroblock ROWS are bands of alternating coloured blocks, and everything below is flat grey
      with faint texture. That is not a frame with wrong coefficients. **That is a frame that lost
      synchronisation partway and then degenerated into DC-plus-EOB for the rest.** A desynchronised reader
      still finds valid-looking codes, so it still terminates each block and still reaches 1440 of them.

      So "decoded exactly" means SELF-CONSISTENT, not correct. Which invalidates the metric everything
      downstream was scored on:

      - The 248 completions are evidence for the container, the geometry, the 10-bit DC and the 2-bit EOB.
        They are evidence for **no AC code at all** — every one of those blocks is DC + EOB.
      - The length derivation used tails collected at the first failure of frames that were, on this
        reading, already mis-parsed. Fitting a width to those tails may have been fitting noise.
      - The escape sweep scoring 248 for all eighteen layouts is consistent with the escape never being
        correctly reached, not with the escape being irrelevant.

      Things that ARE established and survive this: the container and demuxer (frame counts match an
      independent pass), the frame geometry, the DC width, the EOB code, and that the chroma order is not
      the fault (swapping Cr and Cb changes the artefacts' colours and not their structure).

      **What this needs is a reference** — MPEG-1 Table B.14's codes with their run/level column, or a
      known-good STR decoder to compare a single frame's coefficients against. Not because the work is
      hard, but because every self-contained metric available here is satisfied by a wrong table, and I
      have now demonstrated that rather than assumed it.
- [x] 113. **There IS a reference-free validator, `bs_num_codes` is it, and its verdict is that the AC code
      lengths are wrong.** #112 concluded that every self-contained metric available here is satisfied by a
      wrong table. That was also too strong — the frame header carries one that is not.

      **What `bs_num_codes` counts.** FORMATS.md recorded it as "multiple of 32, range 1440..9568, meaning
      unvalidated". 1440 is exactly the block count of a 320x192 frame, which is the clue: if it counts MDEC
      code words at one per block plus one per (run, level) pair, then

          bs_num_codes  ==  round_up_32(1440 + pairs)

      Tested, it agrees on **241 of 248** completed frames in OUTRO1P. The field is now decoded rather than
      merely described, and #15's "meaning unvalidated" can go.

      **Why it is the validator this needed.** The pair count depends on the code LENGTHS and on nothing
      else — not the run column, not the level column, not the quantiser, not the IDCT. And it is per
      frame, so it does not need a reference, a capture, or a known-good decoder. It is exactly the
      discrimination #112 said was unavailable.

      **Its verdict, split by whether a frame carries AC data at all:**

          TAKE1BP   AC frames agreeing:  0 of 3
          OUTRO1P                        2 of 9
          ROGUEINP                       0 of 18

      A DC-only frame agrees trivially — no pairs, and `bs_num_codes` is 1440. **A frame with real
      coefficients essentially never agrees: 2 of 30.** So the AC lengths produce the wrong number of
      pairs, and the derivation in #108 was fitting noise, exactly as #112 suspected but could not show.

      That is a much better position than "unverifiable". The table can now be SCORED without a reference:
      any candidate assignment of lengths is measured by how many AC-carrying frames satisfy their own
      header. A search over the length assignments is the obvious next move, and it has an objective that
      discriminates — which nothing before this did.

      Also worth keeping: only **3 of 1311** incomplete frames in OUTRO1P have already exceeded their
      `bs_num_codes` when they fail. The pair count almost never overshoots before the failure, which says
      the error is a code being read at the wrong LENGTH rather than the reader inventing extra pairs.
- [x] 114. **The headers alone say what the AC codes must average: `len + 1` = 10.5 bits, from two fields
      and no decoder.** With `bs_num_codes` decoded (#113), a frame's own header over-determines its
      bitstream, and the arithmetic needs nothing else:

          bits_used = 1440 * 12  +  SUM over pairs of (code_len + 1)

      — 1440 blocks, each a 10-bit DC and a 2-bit EOB, and every pair costing its code plus a sign. With
      `pairs = bs_num_codes - 1440` the mean falls straight out. Across every AC-carrying frame on the
      disc (`tools/stx_implied_len.py`):

          OUTRO1P   1320 frames   mean 10.52   median 10.49
          ROGUEINP  2443 frames   mean 10.18   median 10.33
          TAKE1BP   1277 frames   mean 10.82   median 10.81

      **Mean code length about 9.5 bits**, tight across three films and 5,040 frames. That is a hard
      constraint on any candidate table and it was free — no decode, no reference, two header fields.

      It also has to be reconciled, and the reconciliation is the next question. Nine and a half bits is
      long for a table whose commonest code is two, which admits two readings: either PSX MDEC content at
      qscale up to 20 really is sparse enough that the surviving coefficients carry large runs and long
      codes, or `bs_num_codes` is not `1440 + pairs` and the fit in #113 was carried by the DC-only frames
      that satisfy any such formula trivially. **The two are distinguishable** — a candidate table whose
      realised mean is 9.5 and which also satisfies the per-frame count is the first reading; one that
      cannot reach 9.5 at all is the second — and either answer is worth more than the guess.

      What this does not do is decode a movie. It narrows the search that #113 made possible, and it is
      recorded because it cost two header fields and pins a number nothing else here could reach.
- [x] 115. **The movies play. Five passes of instrumentation named the wrong fault four times, and what
      settled it was a number in the frame header that had been sitting there unread.**

      #107 through #114 built a demuxer, most of a decoder, and an increasingly elaborate set of instruments
      that each pointed somewhere else: five missing code groups, then a wrong run column, then a
      misconfigured escape, then a biased smoothness oracle, then "there is no self-contained validator".
      **All 5,301 frames of all three films now decode**, and the repairs were two.

      **The table's 8-leading-zero group is SIXTEEN codes of THIRTEEN bits, not eight of twelve.** #108
      derived the long code lengths by bucketing failures on leading zeros and measuring the distinct tails
      after the leading `1`. That method is ambiguous by exactly one bit and nothing in #108 noticed: a group
      of sixteen 4-bit tails each followed by a sign shows the same "N distinct at width w, 2N at w+1"
      signature as eight 3-bit tails each followed by a sign. Four of the five groups came out right and the
      fifth came out one bit short, so every block reaching one of those codes read one bit too few and then
      carried on finding valid-looking codes in mis-aligned data. That is why #112's frames "decoded exactly"
      while showing bands of colour over grey: a desynchronised reader still terminates blocks.

      **The bit reader's lookahead was broken at the end of every frame.** `br_get` returned `v << 1` when it
      ran out of data instead of zero-padding to the width asked for, so a 17-bit peek with fewer than 17
      bits left came back MISALIGNED. Every frame carries 8 to 47 bits of padding after its last block, so
      the last block of every frame peeks past the end — and its EOB never matched. 640 frames of 5,301 used
      to stop exactly one block short, always the last, and the histogram of that said "unmatched, twelve
      leading zeros", which is the one branch B.14 leaves unused: the signature of a reader landing in
      nothing, not of a missing row.

      **`bs_num_codes` is the MDEC's DMA LENGTH, and #113's reading of it was wrong in a way that flattered
      the wrong table.** It is not `1440 + pairs`. It is

          words = 2 * blocks + pairs      (a DC word and an EOB word per block, one word per pair)
          codes = round_up_32(ceil(words / 2))       (longwords, padded to 32 of them)

      A DC-only 320x192 frame is 2*1440 = 2880 words = 1440 longwords, already a multiple of 32 — which is
      exactly the number that made "one word per block" look confirmed on the only frames that could not
      discriminate. Under the correct reading **5301 of 5301 frames agree**, AC-carrying ones included, and
      it is the reference-free validator #112 said did not exist. #114's "mean code length must be 9.5 bits"
      dissolves with it: pairs were undercounted 2.4x, and the real mean is about six, which is what a table
      whose commonest code is two bits should give.

      **And the macroblocks are stored COLUMN-MAJOR.** Left-hand column top to bottom, then the next. This is
      invisible to every consistency check a decoder can run on itself — a row-major read consumes exactly
      the same bits and produces exactly the same blocks — and visible immediately in a picture, as content
      that is plainly real sitting in the wrong 16x16 cell. It cost one line and would never have been found
      by any amount of counting.

      **The escape is measured, not asserted.** With the decoder working, `movie sweep` finally
      discriminates: of eighteen layouts exactly two decode every frame, run 5 / level 11 and run 6 /
      level 10 — the same sixteen bits split differently, which synchronisation cannot separate. The picture
      can, and does: roughness 3.37 against 4.18. The smoother is the MDEC's documented 6/10.

      **What this leaves as the lesson.** Every instrument built in #109-#111 was sound and every conclusion
      drawn from one was wrong, because each was scored on the frames that completed and the frames that
      completed were the ones carrying no AC data. The check that worked was not a better instrument: it was
      a field in the header, which states the answer per frame and needs no decode to be trusted. #113 found
      it and then fitted it to the wrong formula for the same reason — the DC-only frames satisfy any
      formula of that shape trivially.

      **In the client:** `--movie NAME` plays one, and a QENDMIS map that names a film plays it instead of
      the placard, so the campaign ends on the outro rather than on a paragraph explaining that it cannot.
      The player is `src/game/movie.h`: a sector window, the 25 fps the container forces, and the XA in slot
      7 read per sector because those sectors are Form 2 where the video is Form 1. Sound and picture come
      out the same length on all three films.
- [x] 116. **Lava, acid and the laser walls: 89 volumes on the disc that could not hurt anybody, and a
      throttle that had been erased by the actor refresh.**

      Six primitives assert an environment bit and this port has run them since #71. Five more are DAMAGE
      volumes and nothing had ever called them, so the player could stand in lava indefinitely. Read out of
      their handlers, which are as short as the environment ones:

          INACID     0x8002E49C   damage(nobody, ent,  1, mod 9)
          UNDERACID  0x8002E4C8   ent+0x98 |= 0x1100; damage(nobody, ent,  1, mod 9)
          INLAVA     0x8002E500   ent+0x98 |= 0x1000; damage(nobody, ent, 20, mod 10)
          UNDERLAVA  0x8002E53C   ent+0x98 |= 0x1100; damage(nobody, ent, 20, mod 10)
          LASERWALL  0x8002E1F0   damage(ent, ent, item[+20], mod 11)

      Read the same way the environment bits are — once at attach, off each volume's own record — and
      applied on the same per-tick pass, because that is what the volume dispatcher's predicate arm amounts
      to. **45 acid volumes and 44 lava volumes** across the disc: BOSS1 has seven, MATRIX5 eleven, WASTE1
      eleven, POWER1 seven of acid and two of lava.

      **LEVEL-triggered, and that is forced rather than chosen.** The damage function keeps a per-target
      deadline for mods 9 and 10 (`env_next`, client+0x94, 400 ticks and 100), and a throttle only means
      something if the call repeats. Firing once on entry would make lava a single point of damage.

      **The throttle had been unobservable, and the bug is one this project has already had once.**
      `q2_actor_from_player` rebuilds the hurt-actor from the inventory on every hit, and it cleared
      `env_next` along with everything else — so each call reset the deadline the previous call had armed,
      and lava landed thirty times a second instead of three. A full-health player died in under a fifth of
      a second, which reads like the volume test being wrong rather than the throttle being erased. The
      invulnerability and protection deadlines (client+0xB0/+0xB4) were being cleared by the same line and
      are preserved with it. #59's `owner` field was the same fault in the same function.

      Measured, `q2psx-inspect pmove <disc> <map>` now stands the player in each damage volume for three
      seconds of level clock and reports the hits against what the throttle predicts:

          BOSS1   7 lava volumes   9 hits each, health 100 -> -80   (expected 9)
          WASTE1 11 acid volumes   3 hits each, health 100 ->  97   (expected 3)

      Held in place, because these centres are points in space rather than ledges: the first run of it gave
      1, 2 and 3 hits for identical acid volumes, which was gravity being measured rather than the throttle.

      **The two LASERWALLs on the disc cannot fire, and that is a reading rather than a gap.** Its handler
      returns before the damage when the object slot at +18 is negative (`bltz` at 0x8002E220), and both
      calls name an empty slot — `0 armed of 2 declared`. That is the same shape as OBJDRAWOFF's empty
      slots (#78) and the BUTTON in #81: whatever leaves an object slot empty is not confined to one
      primitive.

      **The latching flags are deliberately not asserted.** `sim.c` already records why: the frame's clear
      at 0x8003A25C does not include 0x1000, so that bit is not a per-tick environment and something else
      owns its lifetime. Setting it every tick would set a flag nothing takes back. The damage does not
      depend on it.
- [x] 117. **The route from a shot to a breakable, which is a registry of BOXES and not the collision node
      #67 guessed. Glass breaks now.**

      #66 wired GLASS to the debris burst and measured that no trigger volume on the disc ever calls it;
      #67 said what was still owed was "the route from a shot to a UserFuncs object" and guessed the
      collision node, on the grounds that a hitscan ends on one. It is a separate structure, and following
      it took four reads:

          0x800CAE10  48 records of 64 bytes — DAMAGEABLE BOXES. The byte at +54
                      is the slot's use flag; bit 0x4 is "can be damaged" and
                      bit 0x8 is set the first time something hits it.
          0x800555D8  allocates one, copying the geometry out of a Scene node's
                      own record. Returns the record pointer.
          0x80053AA4  the SWEEP, inside the shot trace: walk all 48, test the ray
                      against each in-use one (0x80052078), and on a hit write
                      hit-kind 2 and the slot INDEX into the trace result at +18
                      and +22.
          0x800488DC  a weapon's impact: read the index back, re-form the record
                      pointer (`sll v0, v0, 6` — the 64-byte stride is what named
                      the array), check bit 0x4, set bit 0x8, and call...
          0x8002EF1C  the ROUTER: walk the 48-entry OBJECT array at 0x800D6BB0
                      (92-byte stride) and for every object whose +0x28 equals
                      that record pointer, store the damage point at +0x00 and
                      call the object's own +0x24 callback. Five call sites, all
                      weapons: 0x80046634, 0x80047E54, 0x800488DC, 0x800492E8
                      and 0x80049B18.
          0x8002A518  GLASS's load constructor, which ties them together: it
                      allocates the box from the pane's Scene node and stores the
                      returned pointer in obj+0x28.

      So a pane's identity to the weapon code is a box in a registry the shot trace tests **separately from
      the world hull**. That is why #67 found no reference to the object array from the weapon code and
      concluded the damage path must arrive with the object in hand: it does, by index, through a structure
      neither side of that search was looking at.

      **The port keeps the mechanism and not the memory layout.** Each GLASS call's Scene node box is
      registered at zone load (through the same two-buffer rebase #66 needed — four of the ten slots read
      -1 in COMMON's copy), the shot is swept against those boxes, and the nearest crossing runs GLASS's
      own body: the hit burst out of the crossing point, the hit-point subtract in the item, and the
      shatter across the whole box. The ray test is a slab test rather than 0x80052078 transcribed, which
      is stated at the function.

      **Both the hitscan and the projectile paths route**, because the console's five call sites are not
      all hitscan — a bolt's is one of them, and a blaster is what a player has in hand when they first
      meet a window. A projectile is tested over the STEP it just took rather than at its impact point: a
      pane is thinner than a step at bolt speed.

      **The same registered box is solid while it is live.** `0x80053C58` walks that 48-slot entity table
      before trigger volumes for movement, and the general point/projectile passes also test runtime entity
      boxes because neither collision hull contains a pane. The port now inserts every GLASS box after mover
      parts, using the Scene record's raw bounds, and disables it in the same fatal-hit call that marks the
      pane broken. Save restore re-synchronises the box from the broken latch. A zero-mover regression test
      is deliberate: glass must arm those passes by itself rather than depending on a door elsewhere in the
      map.

      Measured on LAB, whose two panes bind: standing at 3200,-160,-4478 and firing, `1 SHOT, 11 pieces`
      — 1 from the hit and 10 from the shatter, which is exactly what that item's own operands at +10 and
      +12 say. Nine maps carry a breakable: BASE0, BASE1, BASE3, COMMAND, JAIL2, LAB (two), MAGDEMO,
      WASTE2 and WASTE4.

      *An error worth keeping:* the first implementation matched on the collision node, following #67's
      guess, and bound zero panes on every map — `q2_coll_find_node` returns -1 for a point inside a pane,
      because the hull's nodes are EMPTY volumes. That failure is what sent this back to the disassembly,
      and the answer was two functions away.
- [x] 118. **SHOOTTHEN is a shoot-to-activate switch, and with #117's route in hand it works.**
      #66 declined to run it from a script and was right to: `0x8002E840` returns on a zero damage
      argument, and a script CALL always passes zero. What it does with a NON-zero one is the whole
      primitive:

          if (item.len != 8)  return
          if (damage == 0)    return               ; the scripted no-op #66 found
          item[+6] -= damage                       ; hit points, in the ITEM
          if (item[+6] > 0)   return
          0x80055750(obj+0x28)                     ; free the damageable box
          obj+0x28 = 0
          0x80027950(obj+0x40, 0, item)            ; RUN THE RECORD

      `0x80027950` is the record execution dispatcher this project already knows, and `obj+0x40` is the
      record the item's constructor was building in (cached from gp+16936 at `0x8002BD24`). So SHOOTTHEN
      means: shoot this panel, and when it gives out, run the rest of whatever record it belongs to.

      Four calls on the disc, two of which resolve their object slot without the rebase and both of which
      now fire: `q2psx-inspect pmove <disc> BASE0` shoots node 271 for 100 and reports `1` record raised,
      and WASTE4's node 143 the same. It is the same registry, sweep and router GLASS uses (#117) —
      SHOOTTHEN simply answers with a record rather than with debris.
- [x] 119. **DISABLEME retires the record it is running in, and the one call a sweep can reach is behind
      the key gate — which is how it was never observed.**

      `0x8002EAA8` is four instructions of substance: read the record currently executing out of gp+16936,
      add the Events base, OR 0x80 into its header byte at +3. That bit is the DISABLED flag the record
      dispatcher tests at `0x8002799C` before it runs anything, so the record never runs again. It does NOT
      stop the record — the primitive sets the bit and returns, so what follows it runs this once.

      Expressed the way `TIMER`'s deferral is (#83), as a field an `on_call` hook writes, because the hook
      reports a CALL and has nowhere else to put an answer.

      **Two calls on the disc and only one is reachable, and finding it needed a new flag.** JAIL2's sits in
      a record no trigger volume runs. BIGGUN's is reached — the tool's sweep runs it — but in the client it
      is behind `ONKEYDO`, and a scripted run cannot go and find a key. `--keys` hands the player all twelve
      key bits, which is what makes what is behind the gate measurable at all:

          BIGGUN --fire-triggers            : 0 records retired  (gated)
          BIGGUN --keys --fire-triggers     : 1 record retired

      The difference between those two lines is the gate working and the primitive working, in one run.
- [x] 120. **DISH's speed is the constant ONE, and #81 was reading the wrong half of the primitive.**
      #81 declined to build a DISH because "its operand table names no speed", and #82 left it as the last
      unbuilt mover. The table is right and the conclusion was wrong: there is no operand because there is
      no choice. `0x8002E314` writes an immediate —

          80: if (item.len != 8)          return
          84: obj = objects + 92 * lh(item+6)   ; the object, allocated at load
          88: if (obj+0x52 != 0)          return ; a ONE-SHOT latch
          8C: obj+0x52 = 1
          90: obj+0x44 = (s8)item[+5] << 5      ; the travel
          94: obj+0x3A = 1                      ; THE SPEED, an immediate
          98: obj+0x4E = clock + 300            ; one second, the wait

      — exactly as a BUTTON's speed is one unit a tick. Built now, and the last of the movers is in.

      **Where the node comes from is the other half.** The constructor at `0x8002A8DC` rebases `item+6`
      into the ZONE's copy of the Events chunk and reads the Scene node there, then stamps the runtime
      object index back over COMMON's copy — which is why the slot reads -1 to anything looking at COMMON.
      That is #56's two-buffer rebase, and BASE3 shows it plainly: the DISH is dropped under zones 0, 1 and
      2 with `node -1` and **builds under zone 3 with node 129**. The port asks the rebase for the item's
      own length rather than a constant 20, because an eight-byte item cannot satisfy a twenty-byte request
      and would silently fall back to COMMON's -1.

      The build now also SAYS what it dropped and what it built from — `mover dropped: LIFT1 node 0 speed 4`
      and `built from: MOVER_A/B/C DISH` — because an empty object slot and a primitive the port cannot
      build look identical in a count, which is the point #81 made and could not act on.
- [x] 121. **The game finishes. One run, eleven levels, five units, and the outro plays.**
      Every piece of this had been measured on its own — #88's LOADMAP, #87's MISCOMPLETE, #92's movie
      player, #115's decoder — and the whole had not been run end to end since the decoder existed. It
      does:

          mission: Strogg Outpost   (unit 1)      mission: Powerplant       (unit 3)
          mission: Outer Base       (unit 1)      mission: The Reactor      (unit 3)
          mission: Installation     (unit 1)      help computer: "Locate unit exit and kill all resistance."
          mission: Detention Centre (unit 2)      help computer: "Unit Objective Completed."
          mission: Security Complex (unit 2)      mission: Defence Command  (unit 4)
          mission: Grid Control     (unit 2)      mission: Inner Chamber    (unit 5)
          script says: "Mission Complete."        help computer: "All missions complete. Return to Command Ship."
                                                  MISCOMPLETE: unit 5 over -> EndMission 5 (QENDMIS5)
                                                  mission: Final Showdown   (unit 5)
                                                  movie: 'Extro FMV' plays OUTRO1P.STX
                                                  movie: 1559 frames shown

      `--keys --fire-triggers` is what walks it: a player who has every key and steps into every trigger
      volume on every map. That is not a demonstration of PLAYING the game well — no creature is killed in
      it, and the kill counters read 0 all the way down — it is a demonstration that the level flow, the
      unit boundaries, the mission screens, the objectives and the ending are all connected. The last frame
      is the placard the campaign used to END on, now shown after the film rather than instead of it.
- [x] 122. **A save shut every window the player had broken, which is a defect this session introduced and
      the same one the script flags were carried to fix.**
      `q2_save_capture` carries the script's event flags precisely so a save does not restore a level with
      every door the player opened shut again. #117 gave the port breakable panes with state — hit points
      that count down and a broken latch — and nothing carried them, so a save made after shooting a window
      brought the window back while the shards stayed on the floor.

      A `BRKS` chunk now does, keyed by SCENE NODE rather than by index: the registry is rebuilt from the
      map on load, and an ordinal is stable only while build order never changes. The version is NOT bumped
      — the chunked format exists so a reader skips a tag it does not know, and a save written before this
      simply has no panes in it, which restores them as the map built them.

      `tests/test_save.c` round-trips it through a registry whose two panes are in the OPPOSITE order, so
      the test fails if the match ever becomes positional.

      **And the DOORS had the same hole, with a worse consequence.** The mover set lives in the client
      rather than the sim, so `q2_save_capture` never saw it — a reload rebuilt every door shut. Worse than
      shut: the script flags ARE carried, so the record that opened it has already run and will not run
      again. The door was closed and could not be reopened.

      A `MOVR` chunk carries state, saved state, the block, delay and wait countdowns — all three are
      mutated at run time, not settings — the triggered and announced latches, and how far along its travel
      it is. Keyed by the event item's offset, which `mover.h` already establishes as a mover's identity,
      PLUS which of that item's movers it is: a `MOVER_C` double door is two leaves from one item, so the
      offset alone does not name a leaf. The test builds exactly that case and fails if the sequence number
      is dropped.

      **And the CREATURES, which is the biggest of the three.** A save reloaded into a room the player had
      already cleared, full again. A `CRES` chunk carries who is in use, who is dead, their health, the
      frame they are posed on and where they are standing — parallel to the rebuilt set, whose order comes
      from the map's spawn records and is fixed, and refusing outright when the counts disagree rather than
      putting one creature's health on another.

      *What is NOT carried, stated rather than left to be found:* the AI's own timers and its enemy. The
      rest of `q2_monster` is function pointers and pointers to other monsters; the callbacks come back by
      construction because the module is relocated and bound again at load, but a pointer into a set that
      has been rebuilt cannot be. A creature that was hunting resumes from its stand. Restoring a stale
      pointer would be worse than restarting the hunt.

      Three chunks, all additive, none of them bumping the version — which is what the chunked format was
      built for and the first time it has been used that way.

      **Checked end to end, not only round-tripped.** A data-level round trip cannot say whether the CLIENT
      hands over everything it owns, and all three of these live outside the sim and had to be wired
      separately. `--save-load N` quick-saves at frame N and quick-loads on the next, reporting the world
      either side:

          LAB,   pane shot            BEFORE 1 pane broken        AFTER 1 pane broken
          LAB,   every trigger fired  BEFORE 35 doors moved       AFTER 35 doors moved
          BASE1, a creature killed    BEFORE 1 dead               AFTER 1 dead

      Before this pass all three of those AFTER columns were zero.
- [x] 123. **VIEW CREDITS: the front end offered it and the port bounced off it. The whole roll is on the
      disc, and none of it is a page record.**
      `Q2_MREQ_CREDITS` fell through to `Q2_MREQ_NOT_BUILT` — "that page is not reconstructed yet" — and
      went back to the title. The words were there the whole time: QFRONT's text pool carries the credit
      roll as **101 contiguous strings**, from `HAMMERHEAD LTD` to `DAVID GRIJNS`, ending immediately
      before `PLEASE WAIT WHILE`, which is the disc-swap prompt and the first string past the roll.

      **And it is not a page array, which is the finding.** `q2_levelbin_menu_pages` reads 45 pages and 186
      rows out of this module and exactly ONE of them mentions the credits: the `VIEW CREDITS` row on the
      OPTIONS page, at 256,163. There is no `{text, x, y}` record for a single credit line, so how the roll
      is paged or scrolled, and which lines are headings rather than names, lives in the module's CODE.

      So the port does what it did for the deathmatch scoreboard (#106): **the words are the module's and
      the layout is this port's, and it is marked at the reader rather than left to be discovered.** The
      roll is presented in module order — the order it was written in — as a centred scroll in the game's
      own font. What the port does NOT do is guess the pairing of role to name.

      Anchored on the two strings rather than on offsets, so a build that moves the pool still finds it.
      `q2psx-inspect menu <disc> pages QFRONT` prints the roll beside the pages, so "the words are on the
      disc and the layout is not" is a claim that can be checked rather than taken.
- [x] 124. **The cinematics were decoded and were not WIRED, and the disc says where all three go — including
      the one no table can name.**
      `stx.h` had decoded all 5,301 frames of all three films and the campaign ended on the outro, and that
      was the whole of it: `TAKE1BP.STX` never played, `ROGUEINP.STX` never played, and the outro was started
      from `QENDMIS5` because that was the only place a film could be started from. Four things were sitting
      unread.

      **1. `QFMV` is the map that IS a cinematic.** The level table's records 10 and 11 are `Intro FMV` and
      `Extro FMV` and BOTH resolve to the directory `QFMV` — 45.5 KB, no geometry, no letterforms, no icon
      sheet. One directory, two cinematics, and what tells them apart is the DISPLAY NAME the level was
      entered under, which is exactly what the module compares against its own movie table before calling
      the player:

          if (!strcmp(screen, "Intro FMV")) play("TAKE1BP.STX", 1281, 0x10000, 1, 255);   ; 0x80100958
          if (!strcmp(screen, "Extro FMV")) play("OUTRO1P.STX", 1500, 0x10000, 1, 255);   ; 0x801009CC

      The port had been reading the movie table off whatever map carried it and taking the Extro record,
      which is why the only reachable film was the outro.

      **2. The second argument is a STOP POINT, and every film is cut short by it.** It is stored at
      `module+0x7384` and compared at `0x80102A5C` against the STR frame header's own `frame_number` at
      `+0x08`, with `sltu` / `bne` / `sw 1, 0x7388` — so the frame carrying that number is not shown. The
      disc holds 1,283, 1,559 and 2,459 frames; the console plays 1,280, **1,499** and 2,456. The outro's
      last **59 frames — 2.4 seconds — are on the disc and have never been seen**, and this project had been
      advertising "all 1,559 of its frames" as fidelity when it is 60 more than the console plays. The other
      three arguments are not timing: `0x10000` is the byte size of each of the two VLC buffers, allocated
      by name at `0x80102F0C` / `0x80102F28` and defaulting to `0x20400` when zero.

      **3. `MISCOMPLETE` on the last unit goes to `Extro FMV`, not to `EndMission 5`.** `0x80018ED8` was
      already read and recorded (#88) as loading `"Extro FMV"` on answer 5 — and the port then routed unit 5
      to `EndMission 5` like every other unit, because `Extro FMV` was a name with nothing behind it. It has
      something behind it now.

      **4. `ROGUEINP.STX` is named by no movie-table record because it CANNOT be.** The record's filename
      field is twelve bytes and `"ROGUEINP.STX"` is twelve characters, so there is nowhere for its
      terminator to go. It is a bare literal at QFRONT's `module+0xDC4`, played from a per-frame state
      handler at `0x80101CD0` that counts a store down by the frame delta:

          80101CF4  lhu  v0, 0x2D90(a0)     ; the idle store
          80101D00  subu v0, v0, v1         ; ...minus the frame delta
          80101D0C  bgez v0, +0x128         ; still counting: do nothing
          80101D34  addiu a0, a0, 0xDC4     ; "ROGUEINP.STX"
          80101D38  addiu a1, zero, 2457
          80101D4C  jal   0x8010B2EC        ; play it

      This entry then called that "the title screen idling into the intro reel, which is what a PlayStation
      front end does", and declared **the THRESHOLD not recoverable**: *"`module+0x12D90` is zero in the
      module image and those two instructions are its only reader and its only writer in all 118 KB."*
      **BOTH CLAIMS ARE WRONG AND #126 IS WHERE THEY FALL.** The store has a writer — 150, in a delay slot —
      and the writer is called from the three difficulty records, so the film is a new game's opening and the
      threshold is half a second. The title screen's own idle is a different store entirely and reaches no
      film at all.

      Also found on the way: **QDUMMY and QMAGINTR carry a DIFFERENT, EARLIER build of the shared module** —
      `TAKE1B.STX`, `ROGUEIN1.STX`, `OUTRO1.STX`, unsuffixed, and a third record where the shipped module has
      two. The region letter is appended at mastering time (#30 lists "movie filename suffix" among the NTSC
      build values), and the third record is the reel, which by the time the module shipped had outgrown the
      field and moved to the front end as a literal.
- [x] 125. **There is an encoder now, and it is the strictest check this format reading has been put to.**
      A decoder can be wrong in ways no picture reveals — a quantiser off by a constant, a zigzag transposed,
      the DC's scale folded into the inverse transform — and still produce something that looks like video.
      Building the inverse and making the two meet in the middle catches all of those at once, and it is
      what a port needs anyway to REPLACE a cinematic.

      `src/formats/stxenc.[ch]` inverts the decoder's own tables rather than carrying copies of them
      (`q2_stx_quant_table`, `q2_stx_zigzag_table`, `q2_stx_code_at` are exposed for exactly this reason: a
      second transcription of a 64-entry table is a drift waiting to happen, and a drift here does not
      crash, it just decodes slightly wrong). The forward DCT is the **transpose of the decoder's own cosine
      matrix**, so any scale the decoder folded in is folded in here automatically.

      **The 6,5,5,5 cadence turned out to be the encoder's CONSTRAINT.** It had been recorded as a property
      of the data. It is not: audio takes slot 7 of every 8, which leaves 21 video sectors per 4 frames, and
      6 sectors per frame off a 150-sector-per-second drive is exactly 25.000 fps. So a frame MUST fit its 5
      or 6 sectors and the only free variable is the quantiser — which is why `bs_qscale` is per-frame and
      runs 1..20 across the disc, and why an encoder for this format is a rate controller with a DCT
      attached rather than the other way round.

      **What it scores.** Re-encoding 400 frames of `ROGUEINP.STX` and reading the result back with the same
      decoder: 400 of 400 decode exactly at 1,440 blocks, `bs_num_codes` agrees on 400 of 400, no frame over
      its budget, zero unmatched codes, qscale 3..6, **45.4 dB mean PSNR** and 40.1 dB at its worst frame.
      The audio round-trips sample for sample — which it should, and saying why matters: a signal that came
      out of an XA decoder already sits on the codec's lattice, so the search finds the original's own
      choice. That checks the addressing and the parameter packing and NOT the rate–distortion behaviour, so
      a synthetic tone checks that separately (`tests/test_stxenc.c`, 59 dB and no drift over eight sectors).

      **And the sectors are real ones.** `src/disc/cdxa.[ch]` builds Mode 2 Form 1 and Form 2 sectors with
      the sync pattern, the BCD address, both subheader copies, the CRC-32 EDC and the Reed-Solomon P and Q
      parity — computed with the address bytes zeroed, which is Mode 2's rule and not Mode 1's. The check is
      not the specification, it is the disc: recomputing all **7,712 sectors of `TAKE1BP.STX`**, 964 of them
      Form 2, returns the four and 276 bytes already there in every single one.

- [x] 126. **The third film was never the attract reel, and the reason it looked like one is the whole
      lesson: a store whose only visible instruction is its READER reads as a variable nobody sets.**

      `ROGUEINP.STX` had been wired to a thirty-second idle on the title screen, with the threshold
      recorded as unrecoverable because "`module+0x12D90` is zero in the module image and the countdown at
      `0x80101CF4` is its only reader and only writer in all 118 KB" (#124). Both halves of that were wrong,
      and each was wrong in a way the other hid.

      **There is a writer, and it is in a DELAY SLOT** eight instructions below its own `lui`:

          80101E50  lui   v0, 0x8011
          80101E54  addiu v1, zero, 150
          80101E6C  jal   0x80103414
          80101E70  sh    v1, 11664(v0)      ; -> module+0x12D90, in the delay slot

      A scan that pairs a `lui` with a nearby load or store finds the reader in `0x80101CD0` and stops. The
      pairing is the bug: MIPS puts the store after the branch it belongs to, so the two can be arbitrarily
      far apart in listing order and still be one statement.

      **And the writer has callers.** `0x80101E4C` is called from `0x8010D380`, `0x8010D3A8` and
      `0x8010D3D4` — the EASY, MEDIUM and HARD records at `module+0xEFE4`, `+0xEFFC` and `+0xF014`, whose
      addresses were already written down in this file. Each stores its skill into `engine+0x366` and calls
      it; it arms the 150, hides the five title objects `module+0x12B20` names, and installs the countdown
      as the page hook. **150 of the console's 1/300 s units is half a second**, so the film is the opening
      of a NEW GAME with a short beat in front of it — and when it ends, the same function puts up the
      two-row page `"STARTING"` / `"GAME"` (`module+0x1C`, `module+0x28`) and calls `engine+0x494(1)`.

      **WRONG FROM HERE TO THE NEXT HEADING, AND #127 IS WHERE IT FALLS.** This entry then read the
      pair as a state and routed the reel's hand-off through the intro FMV:

          8001863C  addiu v0, zero, 12       ; game state 12
          80018650  sw    v0, 10836(at)      ; -> 0x800B2A54, "play the intro"
          80018B00  jal   0x800417F8         ; writes "Intro FMV" / "Default" and state 6

      That chain is real and QFRONT is not on it. `engine+0x2C0` is a twelve-frame COUNTDOWN and
      `engine+0x2C2` is the state it enters when the count runs out — `0x8001F964`, the hook the same tail
      installs, is what reads them — so the reel hands over to state **1**, the game, and the twelve is
      how long "STARTING" / "GAME" stays up. The 12 that means state 12 is QLOGOS's, at boot. The intro is
      a pre-menu cinematic and a new game plays one film, not two.

      **The attract loop was a DIFFERENT STORE all along, and it plays no film.** `module+0x12DC0` is parked
      with 9000 by the title page builder `0x8010CEE0` and counted down by the page hook `0x8010C6AC`, which
      resets it whenever `engine+0x2AC` reports a pad. At zero it calls `0x80101B08` — which hides the same
      five objects, installs the one-row page `"DEMO OF GAME"` (`module+0xC`) and hands off. Thirty seconds
      is real, the reset on input is real, and what waits at the end of it is a demo of the game, which this
      port has no player for. Its countdown is therefore gone from the client rather than pointed at the
      wrong film, and this replaces the *"still open on the front end"* note under #113.

      **One more thing this cost, and it is the part worth carrying.** `docs/FORMATS.md` gave the reel's
      call site as `0x8010D5F8`. That address is a four-instruction function that plays `ROGUEINP.STX` with
      the same five arguments and **has zero references in the whole module** — an earlier build's entry
      point, left in the image, sitting a few hundred bytes from the difficulty handlers. It was found by
      searching for the filename and taking the first call site, and it agreed with the story already
      believed, so nothing ever asked it for its callers. The live call is at `0x80101D4C`, inside the
      countdown. **A dead function is indistinguishable from a live one until you ask who calls it** — the
      same shape as #65, where an address hunted for callers turned out to be a label, and #8, where a
      record read as an array of structs turned out to be two arrays.

## The status bar's fourth sub-draw: the pickup caption, and the two fields nobody was filling

`0x800337D0` runs five sub-draws and the port implemented three of them. The fourth is `0x800359C0`, and it
is the reader for the two fields the touch dispatch has been writing since the item path was transcribed —
`client+84` and `client+188`, the effect and its 900-tick deadline (`0x800372F0`). **With no reader, a
player collected a shotgun and the screen said nothing.**

What it does, in the order it does it:

    80035A20  lbu  v0, 84(a3)          the effect; ZERO returns without drawing anything
    80035A38  lw   v0, 16(a0)          a0 = client+172, so this is client+188, the deadline
    80035A40  sltu v0, v0, v1          deadline < level_time
    80035A4C  sb   zero, 84(a3)        expired -> cleared IN PLACE
    80035A50  lbu  v1, 84(a3)          ...and reloaded, so the expiry frame draws index 0
    80035A64  addu v1, a0, t0          &rect[effect]      -> the upper-left icon field
    80035B10  lw   a2, 64(v0)          nameTable[effect]  -> the caption's %s
    80035B14  jal  0x800894B8          sprintf(scratch, "@03BB8^828282|0~8%s~4", name)
    80035B20  jal  0x80043518          ...straight to the markup interpreter

Three findings came out of it, and the first is the one with reach beyond this section:

- **The effect id and the icon rect index are one number.** The same `client+84` selects the rect
  (`* 5 + 0x8009C478`) and the caption. That is the join FORMATS.md §11.1 asserted and `icontable.h`
  retracted — both were half right. The *correspondence* is real; the *mechanism* is the index, not the
  rect record's fifth byte, which is a palette. §11.1 is rewritten.
- **The captions are a 57-pointer table at `0x800AC144`**, one per effect, and they are not the item
  records' model names: `Sshotgun P` reads out as "Super Shotgun". Now in `itemtable.[ch]`, loaded from the
  disc and diffed against the transcription by `q2psx-inspect items`.
- **The caption is not a notification.** It is drawn every frame from the viewport's own hook while the
  deadline holds, at a position carried in the string. The port had it pushed into the four-slot ring,
  where it aged on the wrong clock, stacked under "Selected Blaster" and sat in the wrong corner.

**Resolved — the fifth sub-draw is the upper-right powerup timer.** `0x80035B38` fills fields 13, 14 and
15 from the four expiry words at `client+172/176/180/184`, walking them as quad, invulnerability,
environment suit and rebreather. The first one whose unsigned deadline is strictly after the level clock
wins, and its count is `(deadline - now) / 300` (the module uses the magic multiply `0x1B4E81B5 >> 37`).
It selects rect 40, 41, 42 or 43 by that same memory-order index; no active deadline supplies rect 0 and
draws nothing. The implementation is `q2_statusbar_powerup_state()` plus the status-bar emitter, kept
separate from the upper-left pickup caption because the retail sub-draws are separate too.

**And two item-path defects the same reading turned up:**

- `0x8005998C`, the branch weapons-stay takes, is two instructions: the particle burst and a jump to the
  epilogue. The port hid the entity before the early-out, so a gun left standing by weapons-stay vanished
  for everyone — the rule accomplished nothing but leaving a live entity where a freed one belonged.
- The port's own settle — 180 ticks of `q2_sim_tick` to drop a fresh spawn onto the floor, which the
  console has no equivalent of — ran the entity sweep. A start position within 286 units of a pickup
  therefore COLLECTED it before the first drawn frame, and since the event queue is cleared at the top of
  the next tick, the sound, the burst and the caption went with it. The sweep is now held back while
  settling, which puts the pickup on the first tick the player actually plays.

- [x] 127. **The intro cinematic is a PRE-MENU cinematic, and finding that out took being told the port
      was still wrong. The boot chain in full: two logo screens, four images and a film, all of it in
      front of the title screen the port had been booting straight into.**

      #126 moved `ROGUEINP.STX` off the title screen's idle and onto the difficulty menu, which is where
      it belongs, and then routed the reel's hand-off through `Intro FMV` on the strength of one link:
      game state **12** is the only thing that sets `0x800B2A54`, the request the dispatcher answers with
      a load of `Intro FMV`. That link is real. **The assumption stacked on it was that QFRONT was what
      wrote the 12**, and it never does — there is no store through `engine+0x3AC` anywhere in its 118 KB.

      What QFRONT's tails write is a DELAYED state change, and reading `engine+0x2C0` as a state was the
      error:

          80101DE4  sh 1,  706(v1)      ; engine+0x2C2
          80101DF0  sh 12, 704(v1)      ; engine+0x2C0
          80101E04  sw ...,  656(v1)    ; engine+0x290 = engine+0x27C = 0x8001F964

          8001F978  lh   v0, 704(a1)    ; and 0x8001F964 is the countdown
          8001F980  blez v0, +0x10      ;   not yet: decrement and return
          8001F990  lhu  v0, 706(a1)
          8001F99C  sh   v0, 11816(at)  ;   -> 0x800B2E28, the game state

      So 12 is **twelve frames** and 1 is the state. That is also what keeps "STARTING" / "GAME" on the
      screen for twelve frames before the game loads, and what keeps "DEMO OF GAME" up for twelve before
      state 10 — one mechanism, two callers, and both of them were sitting in #126's own transcript.

      **The 12 that means state 12 belongs to QLOGOS.** Tracing the boot initialiser instead:

          80018DA4  sw v0, 10888(at)     ; 0x800B2A88 = 1, at boot
          80041748                       ; the dispatcher's answer: "QLogos2"
          80101FA0  sh 14, 0(v1)         ; QLOGOS2 ends -> 0x800B2A5C -> "QLogos"
          801021D0  sh 12, 0(v1)         ; QLOGOS  ends -> 0x800B2A54 -> "Intro FMV"

      and QFMV asks for state 6 when the film is over, which leaves no request flag standing, which makes
      the dispatcher fall through at `0x80018B54` to **`QFront`**. The retail order is therefore

          Legal -> Hammerhead -> id -> Activision -> TAKE1BP.STX -> the title screen

      and a new game is difficulty -> half a second -> `ROGUEINP.STX` -> the first map, with no second
      film in it. Both halves of #126's hand-off were wrong in the same direction.

      **The screens themselves.** `QLOGOS2` and `QLOGOS` carry byte-identical 100 KB `COMMON.DAT`s and the
      same 29,988-byte shared module; what differs is `SNDVRAM.DAT`, 432 KB against 84 KB, and what is in
      it: `Legal.lbm` + `HamLogo.lbm` against `IdLogo.lbm` + `ActLogo.lbm`. All four are 512x240 8bpp with
      a 256-entry CLUT apiece — the whole active picture, not overlays on a scene. Each handler runs one
      counter and derives two brightnesses from it, so the second image fades in ACROSS the first's fade
      out; 128 is the GPU's neutral modulation value and a fade is eight steps of 16.

          QLOGOS2  0x80101D88            QLOGOS  0x80101FB8
            t < 8     t << 4               t < 8    t << 4
            t < 258   128                  t < 83   128
            t < 266   128-((t-257)<<4)     t < 91   128-((t-82)<<4)
            d = t-258, same shape          d = t-83, same shape
            hand off at d = 95             hand off at d = 93

      The off-by-one on each screen's first image is the module's: the first reads the raw counter and
      the second a rebased copy, and the rebase costs a frame.

      **Nothing in either module reads the pad.** `engine+0x2AC`, the word QFRONT's title hook tests for
      input, is never loaded in all 30 KB — so on the console the twelve seconds of logos are twelve
      seconds. The port makes a press end the current screen anyway, which is the only thing here that is
      not the disc's, and it is marked as such at the function.

      **What this cost, and it is the same lesson as #126 one level up.** #126's whole point was that a
      negative result about a wrongly-typed premise looks like a discovery — and then it typed
      `engine+0x2C0` as a state word because a 12 appeared next to a 1, without asking who READS it. The
      answer was four instructions away in a function the same session had already disassembled for a
      different reason. Reading a field is not the same as reading its consumer, and only the consumer
      says what the field is.

- [x] 128. **`func_train` exists, it is called `PLATFORM`, and this port had it modelled as a lift.**

      The question was which primitive is retail Quake II's `func_train`. It is `PLATFORM`, and the proof is
      in what its constructor keeps rather than in its name. `0x8002CBB0` subtracts the first object's
      bounding-box centre from the item's absolute VEC3 at `+4` and stores **all three signed differences**
      as full words at `obj+0x00/+0x04/+0x08` (`0x8002CDD0`, `0x8002CDDC`, `0x8002CDEC`), alongside their
      length at `obj+0x44`. Its own per-frame handler — `0x8002C2D4`, installed at `0x8002CD80`, and not the
      `0x80025658` every other family installs — then divides that vector by the length and scales it by
      progress:

          8002C914  lw   v0, 0(s0)      ; dir[k]
          8002C920  mult v0, v1         ; * progress   (obj+0x20)
          8002C930  div  t4, v0         ; / length     (obj+0x44)
          8002C960  sh   a1, 40(sp)     ; -> one s16 per axis, all THREE of them

      so the displacement written down the `+0x30` chain has an x, a y and a z. It is the only mover in the
      engine that is not axis-aligned. #82 and #84 read the same constructor twice and stopped one
      instruction short both times, recording the length and throwing the direction away — #84 even wrote
      "the DIRECTION is not in this operand", which is exactly wrong: the direction is the part of the
      operand that was being discarded three instructions before the answer.

      **What the lift model cost, measured on the one PLATFORM the disc ships.** BIGGUN, `Events+476`,
      resident in zone 2. Its payload reads `origin (-55731, 11143, 289) speed -4 nodes 31, 32, 30, -1
      delay 0 wait 2`, and node 31's box centre in that zone is `(-23727, 3012, 287)`:

          modelled as    one part, node 31, axis Y, target 32767
          actually       three parts, path (-32004, 8131, 2), length 32516

      Nodes 32 and 30 are the carriage's two side walls and were left standing; node 31 was driven straight
      down through the floor by the length of a journey that is 97% sideways. On screen the deck vanished
      four seconds after the trigger and the track it runs on — chevron-marked, unmistakable once the deck
      is off it — was bare for the rest of the level.

      **Three details that are not roundable.**

      *The target is truncated to a halfword and then `abs()`-ed every frame.* The path is 33,020 units;
      `sh s4, 68(s0)` at `0x8002CDB4` makes that -32,516 and `0x8002C794` makes it 32,516. The ENDPOINT
      survives — the scale is exactly 1 at `progress == target` whatever the target is — so this is
      invisible except as duration, and the port's old clamp to 32,767 had it wrong in the other direction.
      The `abs()` per frame is the tell: nothing does that to a field it expects to be positive.

      *Four object slots, at +20/+22/+24/+26.* The ctor loop at `0x8002CD3C` runs `s1` from 0 to 3 stepping
      the slot cursor by two, exactly as LIFT1's does, and stamps -1 into all four beforehand at
      `0x8002CD18`. The path is taken from slot 0 only, before the loop.

      *Blocked, it backs off; it does not wait.* `0x8002CAE0` (opening) and `0x8002CB34` (closing) save the
      state to `obj+0x56`, load `obj+0x4E` with `progress -/+ speed * 150` clamped into `[0, target]` by an
      unsigned compare that catches both ends with one test, and enter state 4 — which does nothing but run
      `0x8006FEB8`, a clamped move-toward, until it arrives, then restores the state. Blocked again
      mid-retreat, `0x8002CB8C` writes the entry state back over the restore and nothing commits. Note
      `obj+0x56` is the SAVED STATE here and a retry counter in the linear handler; the two families
      disagree about that field.

      **Two sounds the family docs said it did not have.** mover.h asserted PLATFORM "happens to play the
      identical set" as the linear movers. It plays those three AND two more, through a different call
      entirely: `0x8002C778` asks for id 13 at volume 1024 on every moving tick and `0x8002C8D8` for id 14
      at volume 3072 on arrival, both `0x80040800(4, id, volume, &pos, 2048, 4096)`. That function walks the
      entity array at `0x800D5C30`, rejects past the outer radius, attenuates linearly between the two
      (`vol * (far - d) / (far - near)`, dropped under 513) and resolves `id` against a 16-record table of
      SPU parameter pairs at `0x8009D420` — `{ const void *a; const void *b; u16 flags; u16 id; }`, ids 1
      through 0x10. There is no mixer in this port, so `travel_sound` records which of the two a tick asked
      for and nothing consumes it.

      **A rider is carried DOWN and not ALONG, on the console too.** The obvious next worry is that a
      player on a train that moves sideways is left behind, and they are — but that is the engine's answer,
      not a gap here. `0x80053E34` hands `0x80052C70` a pointer to `box+0x30`, the s16 displacement
      `0x80051EC0` accumulates on all three axes, and the sweep loads `lh v0, 2(t4)` and nothing else:
      element [1], used twice (`0x80052E28`, `0x80052E3C`). Elements [0] and [2] are never read.
      `trace.h` gave the reason for the vertical-only relative motion as "movers travel on one axis and it
      is always this one", which PLATFORM refutes; the real reason is that the console reads one element of
      the triple it is handed.

      **And a bug found on the way.** `client_node_centre` indexed `scene.nodes` — the borrowed chunk, 52
      raw bytes a record — as an array of `q2_scene_node`. It compiles with a warning and returns whatever
      the stride mismatch lands on; node 31 came back as `(-5240449, 715263, -77568)`. Its only consumer
      until now was the rotator sound, so every turning hatch on the disc has been playing from a position
      several thousand screens away, which attenuates to silence and therefore looked like a rotator with
      no bound node.

- [x] 129. **The MISSION board is not a level-end screen, and the port showed it at every door.** Three
      call edges settle it and each of them is the only one there is:

          0x80021ADC  the tally draw       <- called ONLY from 0x80018944
          0x80018868  one tally frame      <- called ONLY from 0x80018F08
          0x80018ED8  game state 7's arm   <- state 7 written ONLY at 0x8002DCB4
          0x8002DC68  MISCOMPLETE's exec   (UserFuncs binding-table index 31)

      `xrefs` returns exactly one caller at every step, so the board can be reached by nothing but a
      `MISCOMPLETE` — the end of a UNIT. #69 had reasoned from the other end ("LOADMAP is what shows it:
      the screen is the level ending") and that reading is wrong; six rows, a totals line and a title that
      reads `Mission %d - Complete` are what a unit's tally looks like, and this is why.

      **What a LOADMAP does instead, which also names state 2.** `screen.h` listed 2 among the values
      "returned to the outer state machine and NOT identified here". It is the level change.
      `0x8002DCE0` compares the item's map name at `+4` against the loaded map at `0x800E46B4`, and when
      they differ writes the name to `0x800E46C0`, the start-position name at `+16` to `0x800C8CD0`, and
      **2** to the state word. There is no dispatch arm because none is needed: the in-game loop returns,
      and the outer state machine at `0x80018A10` falls through its four request flags to `0x80018BB8`,
      reads the twelve bytes at `0x800E46C0` straight back out, and hands them to the level selector
      `0x8007C54C`. **The transition is the loop going round again**, and a level change is the load and
      nothing else.

      The port now raises no board on a LOADMAP. What it does raise is on a MISCOMPLETE, and the order is
      `0x80018ED8`'s: spin the board until a press, THEN write `EndMission` with the unit digit added into
      byte 11 (`lbu`/`addu`/`sb` at `0x8001900C`..`0x80019028`) or `Extro FMV` on unit 5, and drop to
      state 2.

      **The rows are keyed by NAME and claimed on ARRIVAL.** Filling them in visit order as the level ends
      was the other half of the same misreading. Four functions share the 150-byte array at `0x8009B550`
      and all four match by name:

          0x800222B8(name)  find, else take the first free row; stamp the live counters in
          0x80022210(name)  the reverse, on a save restore
          0x800223A8(name)  write the live secrets_found into the row   (INSECRET's exec)
          0x80022420(name)  write the live kills_found into the row     (a creature death)

      And every map claims its own. `0x800222B8` is an engine export at `+0x474`, and BASE1's LevelBin
      `init` — a 1176-byte stub that does almost nothing else — looks up the key `MapTitle` through
      `+0x124` and hands the result straight to it:

          80100434  lw   v1, 8(s0)          ; the engine block
          8010043C  lw   v1, 1140(v1)       ; +0x474
          80100444  jalr v1
          80100448  addu a0, v0, zero       ; the MapTitle string

      So a row exists from the moment a level is entered and the counters are written into it as they
      move. That is not cosmetic: `q2_save_capture_mission` carries the table, and a row only written on
      the way out saves as zeroes for the level you are standing in.

      **The clear is the port's and is stated as such.** Six rows and nothing in the executable empties
      them — `0x80022498`, sitting in the MISCOMPLETE arm exactly where a clear belongs, is a six-iteration
      loop with no body, and `0x8003DDB8`'s `memset(0x8009B550, 0, 150)` is on the new-game path only. A
      seventh distinct level therefore registers nothing on the console. The `EndMission` modules are the
      only candidate for clearing it between units and this port does not run them, so the port clears on a
      unit change and `q2_mission_register` returns -1 for the overflow rather than pretending.

      **And the board's empty band is filled.** `mission.h` called the two centred lines under the title
      "the remaining gap", because what fed them was a `0x800701B4` lookup whose key had not been read.
      `0x80021FD8` builds it: `sprintf(buf, "Unit%dMiss1", *0x800B2E20)` — the same key `leveltext.h`
      already calls the objective and the briefing already shows as "Mission Objective". Its caller
      `0x80022094` hands the string to the wrapper at `0x800220C8`, whose rule is a cut and not a column
      count: under 36 characters the whole string is line one; at or over, the LAST space is cut
      repeatedly until the head fits, and the remainder — leading space and all, since `0x800221E4`
      passes `src + len` — is line two.

      **The campaign now goes through the unit boundaries instead of past them.** #88 had a unit's last
      level carrying both primitives and let the LOADMAP win, "otherwise the unit end clobbers the level
      exit and a scripted run stops at BASE2". The console's order is the opposite — the MISCOMPLETE arm
      writes its destination over whatever the LOADMAP left — and the reason a run used to stop is that
      `QENDMIS<N>` is drawn by its own module, which this port reads and does not run. So the port lets
      the MISCOMPLETE win, as the console does, and **carries the LOADMAP's destination across the
      end-of-mission screen** to continue to when it is dismissed. That last step is the port's and is
      labelled at the site: nothing read says the console gets there this way, only that it gets there.

      One `--keys --fire-triggers` run from BASE0, which is a player who has every key and steps into
      every trigger volume on every map, makes fourteen transitions and four unit boundaries:

          Strogg Outpost -> Outer Base -> Installation
              tally: mission 1 complete, 3 levels, secrets 7/8 -> EndMission 1 -> Detention Centre
          Detention Centre -> Security Complex -> Grid Control
              tally: mission 2 complete, 3 levels, secrets 5/5 -> EndMission 2 -> Powerplant
          Powerplant -> The Reactor
              tally: mission 3 complete, 2 levels, secrets 4/4 -> EndMission 3 -> Defence Command
          Defence Command
              tally: mission 4 complete, 1 level,  secrets 1/1 -> EndMission 4 -> Inner Chamber
          Inner Chamber -> Final Showdown

      Ten level changes with no board and four unit ends with one, where the same route used to put a
      board at all ten and reach an `EndMission` screen at none of them. The rows accumulate per unit and
      reset with it: three, three, two, one, matching the levels the sweep's own route actually visited.

      The FIFTH boundary is not in that run and the reason is the sweep's, not the transition's. BOSS2's
      MISCOMPLETE is in zone 2, and firing every volume of zone 0 at once takes the zone gate first — the
      re-arm is per map change, so zone 2's volumes are never queued. Fired where it lives it is the whole
      ending:

          q2psx --map BOSS2 --zone 2 --keys --fire-triggers 60 --frames 1200
          MISCOMPLETE: unit 5 over -> Extro FMV (QFMV)
          tally: mission 5 complete
          movie: playing OUTRO1P.STX, 1499 frames

      It also takes about 900 frames to arrive, so a short run reads as "it did not fire" when what
      happened is that the record it sits in is deferred.

- [x] 130. **The player's death was one frame long, and the console gives it five functions.** What the
      port had was `if (health <= 0) { score it; open page 41; }` — the first tick of a death and nothing
      after it. `0x800396AC` is not the end of the subject, it is the start of a chain, and the chain is
      handed along through the entity's think pointer at `+0x3C`:

          0x8003A1C8  the player think   the gate at 0x8003ADB8, and the handler's only caller
          0x800396AC  player_die         runs ONCE, because it overwrites +0x3C with the next one
          0x80039550  corpse_think       the body, every tick
          0x8003E238  respawn_think      the corpse timer, deathmatch only
          0x8005B358  body_fade          the body shrinking out of the world
          0x8003CE14  player_anim        which of the player's ten moves plays

      Six things in it are behaviour the port did not have, and four of them are visible.

      **Not every death makes a sound.** `0x80039728` is `bne s1, -1` around the sound call, and `s1` is
      the killer byte at `entity+222` — so the death cry is raised only for a death **nobody is credited
      with**. A player shot by another player dies silently. The port had been raising `pla_death4` from
      `update_pain` for every death, because `update_pain` is where it first noticed a health crossing,
      and on the console `update_pain` is never even reached on the killing tick: the gate's arm ends
      `j 0x8003B014` and skips the rest of the think. The rule composes with `0x800396CC`'s correction —
      acid and lava erase the attacker first — so dying in the level's own hazards IS audible however you
      came to be standing in them, and being clawed by a Berserk is too, because a creature is not a
      player either.

      **A single-player death walks back to the front end by itself.** The handler's last block is
      `if (state == 1 || state == 2) { state = 3; deadline = clock + 1200; }`. `0x80041D30` spends that
      deadline every frame and only pushes it out again while the state is *not* 3 — so setting 3 is what
      arms it rather than what disarms it. When it expires it writes game-state request **8**, and 8 is
      `0x8004149C`, which stores `"MagazineExtrQFront"` as the next module. Leave the death screen alone
      and the console goes back to the title on its own. Nothing in the port did.

      **The engine never respawns anybody.** `corpse_think` in deathmatch waits for the death animation to
      finish (`entity+0x102`, whose bit 0 `0x8003DF90` raises when the frame cursor walks past the move's
      end), flattens the body's collision box to 143 on Y, and hands it to `respawn_think` — which counts
      the 1500 at `+0xF4` away and then installs `body_fade`, and `body_fade` takes `dt << 4` off the
      light intensity at `+0xFC` until the model is released. That is the whole of it: the body animates,
      lies there for five seconds and darkens away. `0x8003DDF8` is the only thing on the disc that puts a player
      back, and it has **one caller** — `0x8003DECC`, the mode gate this port already carries as
      `q2_mp_may_respawn` — plus one materialised constant, slot 12 of the engine block, which is
      QMULTI.C's. Respawning is the map module's decision and the engine's death chain has no opinion.

      **The player has an animation set, and it is not a creature's.** Ten names, looked up in order at
      `0x8003C5F8..0x8003CBFC` and living twelve bytes apart from `0x800AC554`: `Stand`, `Run`, `Attak`,
      `Death 1`, `Death 2`, `Death 3`, `Jump`, `Pain 1`, `Pain 2`, `Pain 3`. The six ids the code passes
      map onto them as 0 STAND, 1 RUN, 2 JUMP, 3 PAIN (`rand() % 3`), 4 DEATH (`rand() % 3`), 5 ATTACK —
      note that the ids and the pool are in different orders, which is why the table has to be keyed by
      name. Two rules are stated twice each in the function and both matter to a death: a death move is
      never replaced by anything, and a pain move holds until it has played out unless the request is
      DEATH.

      **`entity+0x44` has two lives, and the first one is the gun in your hands.** The first version of
      this entry called it "a model pointer, which is id's remove linked weapon model" — reasoning from
      the analogy rather than from the code. It is more specific than that: `0x8004EE0C` opens
      `s6 = self->[68]` and then `s7 = s6->[12]`, so the field holds a whole second ENTITY with a client
      block of its own, and `0x8006D280` detaches it, unlinks it and pushes it back onto the free stack
      at `0x800B2BAC`. **The view weapon is an entity and the death handler frees it.** The same word is
      then written with **-40** and read by `corpse_think` and `respawn_think` as the gib threshold. And
      `client->[0]->[288] = 0` is the 784-byte player record at `0x800D5C30 + i*784` forgetting its
      entity, the back-pointer `0x8003B474` wrote at spawn.

      **RESUPPLY was a second RESTART.** `0x8001FF0C` is `*(u8*)0x800B335D -= 1` and it is the only write
      to that byte in the executable; the byte is the "Continues" count the death page's middle row greys
      itself on (`0x8001D774`, and the debug line the handler prints is literally `"Continues %d
"`).
      The port had both rows calling `client_load_zone` and nothing else, so the count never moved. It is
      BSS on the console, so it starts at zero and the row starts greyed — which is why the decrement can
      never underflow there, and why the port clamps rather than wrapping a byte to 255. `--continues N`
      seeds it so the path is reachable.

      **And one defect the rework removed rather than added.** `client_score_deaths` already walked all
      four players and attributed each death from that player's own `last_attacker`; the inline death
      block then scored player 0 **again**, with a hard-coded killer of `-1`. In a deathmatch a local
      player who was shot therefore lost a frag to a fabricated suicide on top of the correct award to
      their killer. The scoring now happens in one place.

      `q2psx-inspect death` checks the reconstruction against the disc's own executable — every constant
      compared as an encoded instruction word, not as a disassembly — and `tests/test_playerdeath.c` pins
      the behaviour: which deaths cry out, that the handler is a one-shot, that a single-player body lies
      where it fell for ever while a deathmatch one dissolves on schedule, and that -40 exactly gibs.

- [x] 132. **The three things #129 left open, closed — and two of them were port inventions rather than
      gaps.** That entry ended with a list: what raises the arrival briefing, what the tally board is drawn
      over, and what clears the six rows between units. All three are readable.

      **There is no arrival briefing.** The port had TWO state machines around ONE console screen —
      `briefing_open`, raised by the zone load, and `popup`, raised by the script — and both draw through
      `q2_briefing_build_ot`, because they are the same panel. The console has only the second.
      `0x80021250` sets the two fields, `0x800213B0` raises them, and every caller of either is a script
      primitive (`0x80023894`, `0x8002BBF4`) or the pause menu's MISSION row (`0x800203AC`). Nothing in
      the transition path touches them.

      What the outer state machine DOES run on a new level's first frame was the thing worth chasing, and
      it is not this. `0x8003E730` reads the exit code the transition carried in `0x800B271C`, and on 2 or
      4 — a level change — calls `0x800203C4`, which installs two sixteen-entry tables at `0x8009B3B4`
      and `0x8009B3E4` through the overlay's own function pointer at `0x800B2FE4+512`. That is the
      notification layer being re-seeded, not a panel being raised.

      So the panel a player sees just after arriving is a trigger volume near the spawn calling
      HELPCOMPUTER, and a map with none shows nothing — which is coherent, because the two fields are
      global (`0x800B27A4`/`0x800B27A8`, "deliberately not per level" already) and the orders stand until
      something changes them. Measured, with no trigger fired and 400 frames each: **BASE0, POWER1 and LAB
      raise it at level start on their own; BASE1, BASE2, JAIL2, JAIL3, SECURITY, COMMAND and BOSS1 do
      not.** The port raised one on all ten.

      `Q2_INTERMISSION_WINDOW` goes with it. It was ten seconds, a port constant invented so a player who
      pressed nothing was not stranded on the tally, and inherited by the briefing when the two shared a
      release. The tally waits for its press as `0x80018ED8` does, and the pop-up has the fifteen seconds
      every `0x800213B0` call in the executable passes it.

      **The board is drawn over the world, and the port already had that right.** `0x80018868` does not
      call the viewport walk itself, which is what made it look like a cleared screen — but its tail
      `0x8003915C` calls `0x800780C0`, the screen build `screen.h` documents as the thing that "decides
      before anything is drawn" whether to clear, and the polygon emitters at `0x80064F10`/`0x800651D4`
      with it. It is an ordinary frame with the board on top and the frame delta zeroed at `0x80018928`,
      which is exactly what `client_intermission_frame` does.

      **Nothing clears the six rows between units, and the port's clear was the last guess in this
      screen.** The only clear in the executable is `0x8003DDB8`'s `memset(0x8009B550, 0, 150)`, and it is
      the `else` arm of `0x8003D62C`: that function looks up a block by the key `"PlayerSave"`
      (`0x8007FBEC`) and, when it finds one, copies six 25-byte records out of it at `+0xD4` into the
      table and hands the loaded level's counters back through `0x80022210`. No block, no restore, and the
      table is emptied. So it is cleared on a NEW GAME and at no other time. The one place a per-unit
      clear belongs — `0x80022498`, in the MISCOMPLETE arm between the board's setup and its spin — is a
      six-iteration loop with no body, and no engine export hands a level module the array's address, so a
      module cannot do it either.

      The consequence is the original's and the port now reproduces it rather than tidying it: six rows,
      first six distinct levels, and a seventh registers nothing. A player reaches unit 2's board having
      visited exactly six levels, so that board is full — and unit 3's shows the same six under a
      different title. A board that lists a unit's own levels looks more like a feature and is a screen
      the console does not draw.


## RETRACTION and replacement: the gun's residual is the ARM, and it is not the lighting

An earlier version of this section concluded that the view weapon's remaining error was a
BALANCE between the gathered lights and the ambient floor — "too much back colour under too
little directional light". That conclusion is withdrawn. It was drawn from one level, and a
second level disproves it.

**What still stands, and is checked in:**

- **`fa11948`'s next step is answered and is a dead end.** `+0xFC`/`+0xFE` cannot separate
  lights from ambient, because `q2_light_env_build` scales the light directions by
  `intensity_a * intensity_b >> 11` AND the back colour by the same product `>> 24`
  (`0x8006B468`). Swept: every value below 4096 darkens both materials at once.
- **The gather is exonerated four ways over.** view-space list with a node, view-space with
  none, world list with no node, and gathering at the WEAPON's origin rather than the
  player's — all measured, none better than what ships.
- **The whole lighting chain now verifies against the executable, step by step:**

      colour per light   0x8006AE30..0x8006AEDC   (rgb_byte * atten) >> 8, truncating
                                                  toward zero, then clamped to 4096 —
                                                  `q2_light_set_add` character for character
      attenuation scale  0x8006AE14               `a2 = 64`, which is Q2_LIGHT_ATTEN_SCALE
      colour matrix      0x8006B1E0 onward        light rgb copied VERBATIM from the record
                                                  at 0x800DDC60 + i*16, halfwords +2/+4/+6;
                                                  nothing scales it on the way in
      GTE stage 1        light matrix . normal, >> 12, IR clamped at 0 (lm = 1)
      GTE stage 2        (BK << 12) + colour . IR, >> 12   — the hardware's own shape

  There is no missing factor in any of it. The port's arithmetic is the console's.

**What replaces the conclusion.** Render the BASE0 spawn twice, once with every model face
suppressed, and the difference is an exact weapon mask; split it into the flesh forearm and
the metal body, erode twice so an alignment error cannot reach outside either shape, and the
ratio between the two materials is:

                       port arm/body     retail arm/body
      BASE0                 0.819             1.224          (matched location)
      BASE1                 0.647             1.147          (different room, different gun)

**Retail's ratio is nearly light-INVARIANT** — 1.224 in an outdoor canyon under a violet sky
and 1.147 in a dark industrial room, two environments with nothing in common. A ratio set by
the lighting cannot do that. It is set by the MATERIALS, and the port's is wrong by a
consistent ~0.6 in both.

**The error is localised to the ARM.** At BASE0, the one place the port and the capture stand
in the same spot:

      body   port 54.5   retail 50.5    +8%   — right
      arm    port 44.7   retail 61.9   -28%   — wrong

and the arm's HUE is exact: forearm B-G is +3.9 in both. So the arm renders the right colour
28% too dark, in a frame where the body beside it is correct, through a lighting path that
has just been verified instruction by instruction.

**Two more things were swept and are now positive results rather than assumptions.**

- **The normal component order is confirmed a second, independent way.** All six
  permutations and all eight sign combinations — 48 variants — were rendered and scored on
  the arm/body ratio against the capture. The port's `(s4,s5,s3)` reading wins outright
  (0.819; the best rival is `(s5,s4,s3)` at 0.917, and no sign flip improves on any of
  them). That order was previously established by geometric correlation alone at +0.75;
  it now also wins on a criterion that has nothing to do with face normals.
- **A global CLUT-index offset is not the fault.** Swept -2..+2: every non-zero delta
  brightens the arm and darkens the body together (delta +1 puts the arm at 62.3 against
  the capture's 61.9, but the body at 28.6 against 50.5). Whatever is wrong is not a
  uniform shift of the model CLUT base.

**AND THE DEFICIT IS NOW BOUNDED BY ARITHMETIC, WHICH SETTLES WHERE IT CANNOT BE.**

At the BASE0 'Default' spawn the gather reaches exactly ONE light — `lit` reports
`node 5, reach 1, kept 1` — with slot colour (1163, 581, 1328). With the back colour at
0x30 that fixes a hard CEILING on what any vertex of the weapon can come out as:

    rgb[c]_max = 48 + colour[c]/16   ->  (120.7, 84.3, 131.0)
    modulate   = rgb/128             ->  R x0.943   G x0.659   B x1.023

Rendering the weapon UNLIT gives its texture directly (tint 128 is unity), so the factor
each material actually needs is measurable:

                  texture        port lit      factor      retail       needs
      body    (94.8 94.8 94.8)  (63.2 47.6 67.8)  R 0.667  (57.9 44.9 60.3)  R 0.611
      arm     (78.9 58.6 42.4)  (67.5 34.2 38.9)  R 0.856  (89.3 49.4 54.2)  R 1.132

**The arm needs R x1.132 and the port's ceiling is R x0.943.** The console's arm is brighter
than the port's maximum possible output at this spawn — not mis-oriented, not mis-shaded,
unreachable. The port's arm is already at ~90% of its own ceiling and the body at ~72%, so
neither is badly lit; there is simply not enough light in the room.

**The hue is exact on both materials**, which is what makes this precise rather than
suggestive: the arm's B-G is +4.7 in the port against +4.8 in the capture, and the lit hue
comes out right from a TAN texture under a violet light. So the light's COLOUR balance is
right and only its total is short, by about 1.24x.

**Everything between the light record and the pixel is now verified and cannot supply it:**
the per-light colour at `0x8006AE30`, the attenuation scale `a2 = 64` at `0x8006AE14`, the
verbatim colour-matrix copy at `0x8006B1E0`, both GTE stages, the normal decode (48 variants
swept), and the light node itself (swept — every node either reaches the same single light or
none). Two fixes made this session moved the arm/body ratio the right way and neither caused
it: 0.730 with the old fan and truncated UVs, 0.764 with the diagonal fixed, 0.819 with the
rounding fixed, against the capture's 1.224.

**So the remaining deficit is in DATA, not code, and it is one of exactly two things:**

1. the SpaceLights the gather reaches at this spawn — the console has more light energy here
   than one attenuated lamp of (1163, 581, 1328), or
2. the arm's texture as decoded — `Blaster G` part 0, page 1, CLUT 91 — being about 20%
   darker than the console's.

Nothing else can close a factor the ceiling forbids. Distinguishing the two needs the console's
own texture or its own light list; a capture of the finished frame cannot separate them,
because a brighter lamp and a brighter texel produce the identical pixel.

**THE ARM'S OWN DATA IS NOW LOOKED AT DIRECTLY, AND IT IS RIGHT.**

- **The UVs land on the arm.** Dumping VRAM page `tpage 0x0032` decoded through CLUT 91 as
  a 256x256 image and drawing the two big arm faces' UV footprint on it (u 53..114,
  v 64..98) puts the box squarely on the forearm-and-fist in the atlas. The mapping is not
  sampling the wrong part of the sheet.
- **CLUT 91 is a clean ramp, and it is TAN.** Sixteen entries, monotonic, luma 85.4 down to
  8.0, no corruption and no stray hue: `A14D 9D2C 9D2B 990B 94EA 94E9 90C8 90C8 8CA7 8CA6
  8C86 8C85 8C85 8C64 8C63 8421`. The right PINK arm comes out of that tan under the violet
  lamp, which is why the lit hue matches the capture to a tenth.
- **The light direction's sign is right.** Negating it collapses the weapon onto the back
  colour (body -34.5%, arm -65.9%), so the port is not shading it inside out.
- **The node is not wrong, and cannot be.** Swept 0..300: node 5 is the ONLY one whose lamp
  is within range of this spawn at all; every other node leaves the weapon on the back
  colour. There is one lamp near the BASE0 start and the port has it.

**Which exhausts the search.** The record layout (28 bytes), the attenuation function
(`0x80076040`, instruction for instruction including the shift of 6, the scale of 64 and the
`den == 0 -> 4096` exit), the per-light colour, the colour matrix, both GTE stages, the
normal decode, the UVs, the palette and the node are each verified. The port's arithmetic
and the port's data handling are the console's.

What remains is a bare inequality: with one lamp of (1163, 581, 1328) and a back colour of
0x30, the arm cannot exceed R x0.943 of its texture, and the capture shows R x1.132. That
gap cannot be closed by anything in mesh handling, UV mapping, rasterisation or the lighting
code, because none of them can produce a value above the ceiling those two numbers set.

**And the player's position is excluded as well.** The capture shows no objectives board, so
it is taken after the level start and the player need not be exactly on the spawn. Walking
the player up to 1200 units straight at the lamp moves the arm from 44.7 to 45.9 against the
capture's 61.9 — it barely responds, because it is already at ~90% of the ceiling and the
attenuation is near its maximum. The body meanwhile climbs from 54.5 to 58.1 and overshoots.
No position reproduces the capture.

**AND THE SEARCH IS NOW EXHAUSTIVE OVER THE WHOLE LIGHTING MODEL, NOT JUST ITS INPUTS.**
The model has exactly three degrees of freedom — the light's DIRECTION, its SCALE, and the
ambient floor — and all three are swept:

    direction   40 directions over the sphere   arm/body peaks at 1.0216 (capture 1.2240)
    scale       x1.5 .. x5 on the light colours  no pair reaches both materials
    ambient     0 .. 0x60                        monotonic; trades body against arm

The direction row is the load-bearing one and is a BOUND rather than a sample. Forty
directions on a sin-weighted spherical grid put the achievable arm/body ratio at most
1.0216 against the capture's 1.2240. The bound extends to any NUMBER of lights: each
light's contribution has its own ratio, a sum of contributions has a ratio between the
smallest and largest of them, so three lights cannot exceed what one best direction gives.

**Overdraw is ruled out too**, which was the last way the inputs could all be right and the
output still wrong. Rendering the weapon with ONLY its two big arm faces (part 0, faces 11
and 13) and diffing against a no-face plate gives the exact pixels those faces own — 706
after eroding twice. Their luminance is 49.69 whether the other 72 faces are drawn or not,
bit for bit, so nothing is painting over the arm. On those same 706 pixels the capture reads
67.62. The arm faces themselves are 26% dark.

And it is not the UV corner order either. All seven orderings of a face's four UV corners —
identity, mirror in U, mirror in V, 180, and the three rotations — leave the arm between
42.2 and 46.1 against the capture's 61.9. The arm samples the region it samples.

The best of the whole space leaves body +15.8% and arm -8.1%, worse on the body than
shipping today (+7.9%), and the arm NEVER exceeds 53.3 against the capture's 61.9 under any
direction whatsoever. So the deficit is not reachable by lighting at all — not by the right
lighting and not by a fitted one.

Which forces the conclusion the other way round: **with this port's textures and normals,
retail's arm brightness cannot be produced by any illumination.** The arm/body ratio the port
can reach is bounded near 1.0 (0.819 today) and the capture is 1.224. A ratio is a property
of the two materials, and no light acting on both can push it past what their texels allow.

So the arm's TEXEL VALUES on the console must be brighter than this port decodes from the
same disc — even though `q2psx-inspect textures` decodes all 553 images with zero failures
to exactly their expected sizes, the page renders as a coherent atlas with a recognisable
forearm exactly under the arm's UV box, and CLUT 91 is the only tan ramp in its
neighbourhood with a clean monotonic fall from (104, 80, 64).

That is the contradiction the next reader inherits, and it is a sharp one: every input is
verified and the output is still 24% short. Settling it needs the console's own VRAM after a
BASE0 load — an emulator dump of page 0x0032 and CLUT 91 — against which this port's decode
can be diffed texel for texel. A capture of the finished frame cannot do it, and five passes
over the renderer have now established that nothing in the renderer can either. The frame matches on geometry, position, size, silhouette, palette and hue;
it is short only on how much light is in the room.

One inconsistency found on the way and worth fixing on its own terms, though it is inert
today: `q2_vram_get_clut4` applies the loader's STP fix-up from `0x800762B4` (every non-zero
CLUT entry gets bit 15) and `q2_vram_upload` — the one that actually fills the VRAM the
renderer samples — does not. It changes nothing while the port ignores per-texel STP and
every model face is opaque, but the two functions disagree about the same data.

That is a texture or palette question, not a lighting one. The arm is `Blaster G` part 0,
page 1, `face.texture` 23, which with BASE0's `clut4_count_a` of 68 is **CLUT index 91** —
and the CLUT word the port binds, 0x4583, decodes to VRAM (48, 278), which is exactly what
`q2_vram_clut_word(91)` should give, so the INDEX is not the fault either.

**So the open question is now: does CLUT 91 as UPLOADED (vram.c) match the disc, and is the
arm's page-1 texel data being decoded at the right brightness?** That is the same remaining
candidate the Soldier's violet patch was narrowed to — "the CLUT's own contents as uploaded
(vram.c) or the disc's texture being what it is" — and these two should now be treated as one
question rather than two. Nothing in the gather, the intensities, the glow, the normals (all
unit, 4095.3 mean over every part of `Blaster G`) or the GTE reaches it.

- [x] 131. **Nobody drives a corpse, and #130 shipped without it.** Reported from play against the death
      chain #130 built: *"players can move around on death and still show view weapon"*. Both are real and
      both are the same omission — the port reproduced the death handler's data and not the one thing it
      does with a pointer.

      `player_die` ends by writing the corpse think over `entity+0x3C` (0x80039818), and **two things live
      inside the player think and nowhere else**:

          0x8003A4A4   the pad read     — 0x80019154's ONLY call site
          0x8003AD98   0x8004EE0C       — the view weapon driver's ONLY call site

      So a corpse stops being steered and stops holding a gun the instant the think is swapped, and
      **neither is tested for anywhere**. There is no `if (dead)` in front of the movement code because
      the console does not need one. `xrefs` gives exactly one caller at each step, which is what makes
      this a reading rather than an inference; the seven DEAD-bit tests that DO exist in the executable
      (0x80038654 and 0x80038794 the camera roll, 0x8003A8B4 the jump, 0x8003AB08 the strafe roll,
      0x8003ADC4 the gate, 0x8003D260 the splash, 0x80045978 the mover) are every one of them about
      something else.

      **The gun is an entity.** `0x8004EE0C` opens `s6 = self->[68]; s7 = s6->[12]` — `entity+0x44` is a
      pointer to a second entity with a client block, not a model handle — and `0x800397F8` hands it to
      `0x8006D280`, which detaches it (0x8007F12C), unlinks it (0x8006A1C4) and pushes the pointer back
      onto the free stack at `0x800B2BAC`. The port now stops drawing it on the same condition, and #130's
      "a model pointer, which is id's remove linked weapon model" is corrected above: the analogy was
      right about the effect and wrong about the thing.

      **What the port had to say out loud.** One tick function, no think pointer to swap: `q2_sim_tick`
      substitutes a neutral pad for a dead player. That alone was not enough and the test caught it — a
      neutral pad only stops `yaw_rate` and `wish` GROWING, and a body that died mid-turn went on turning
      for a second after it fell, because `integrate_look` (0x8003A990) integrates the rate and the rate is
      state. The accumulators the think would have been writing are cleared with it: the two look rates,
      the wish, `recentring`, `autocentre` and `jump_hold`. `vel` is deliberately left alone — that is
      momentum, and `corpse_think` is supposed to inherit it and take it down with its own `dt * 5`.

      Measured on JAIL2: before, a dead player walked the level with a blaster in front of the death
      screen; after, the eye holds `2816 -1216 23831` for the whole 1200-tick countdown, the roll eases to
      `3736` (the death cam's -384), and the gun is gone on the tick the handler frees it. The test asserts
      it by DIFFERENCE rather than by an absolute position — two identical corpses, one handed the stick at
      full deflection and one handed nothing, must end twenty ticks in the same state — because the body is
      still falling and an absolute assertion would have been asserting the mover, not the input.

- [x] 133. **Skipping the intro film with the jump key made the player jump when the level started, and
      the hole was in the pad's ROLL rather than in the level start.** Reported from play: the press that
      dismisses one screen was still being read as a press by whatever came next.

      **The console has no gap to have this bug in.** Its pad record is a pair — `0x800D5EDC + p*0x310`,
      `+0x0C` what is down now and `+0x10` what was down last frame — and the tail of `0x80019154` derives
      every edge from the two as it finds them:

          80019A08  lw   v0, 12(t1)        ; current
          80019A10  and  v0, v0, mask
          80019A14  beq  v0, zero, ...     ; not down at all -> no edge
          80019A1C  lw   v0, 16(t1)        ; previous
          80019A28  sltiu a2, v0, 1        ; edge = down now AND not down before

      The pair is rolled by the frame, not by the world, so it keeps rolling while a film or a board or
      the front end owns the screen. A button carried across a boundary is therefore already `was` by the
      time anything looks at it, and raises nothing. The port rolls the pair inside its input function,
      and that function is one arm of the main loop's dispatch — so every other arm is a gap, and the
      first tick back read `prev` from before the gap against `buttons` from after it. Jump is the loudest
      case because bit 22 exists solely as an edge and is consumed solely inside the tick, but fire and
      both weapon cycles had the same hole.

      **What the console does have is the same idea one button at a time**, which is worth recording
      because it is what a page does when the press that worked it must not be seen twice:

          0x80021920   closing a page masks CROSS (0x4000) out of the live pad word at +0x0C and +0x04
          0x800214C0   raising the objectives panel clears +0x0C, +0x10 and +0x04 outright

      Neither is a level-start mechanism and there is no level-start mechanism: `0x8003B040`, which builds
      a fresh player, sets the loadout and touches no input state.

      So the port grows `q2_pad_roll` and `q2_pad_roll_resume`. The resume seeds the PREVIOUS word with
      what is physically held, which is exactly "nothing carried in counts as a press" — and only that: a
      button tapped and released during the gap is in the accumulated word but not in the held one, so it
      still edges, and a button that is merely held still reads as held, which matters because swim-up is
      the same button as jump and holding it has to work from the first tick.

      **The flag has to be STICKY and that is the part worth writing down.** The input function runs every
      frame but the roll only happens on the frames that TICK, which above 25 Hz is a minority of them
      (#the roll's own note). A gap noticed on a non-ticking frame is forgotten before any roll can act on
      it, and the next frame looks contiguous — so the defect would survive on every boundary that
      happened to land off a tick. Raised on detection, cleared only where it is consumed.

      Measured: `--map BASE2 --keys --fire-triggers` reports `pad: 2 resumes`, one for the tally board and
      one for the end-of-mission placard; `--new-game`, which is the reel and then level one, reports
      `pad: 1 resume` — the boundary the report was about. `tests/test_pad.c` pins the behaviour from both
      sides, including a case that reproduces the defect: an ordinary roll on a fresh pair DOES jump, and
      that is what the level start used to do.

- [x] 134. **The BASE1/BASE2 parity cluster was seven independent state mistakes, not one missing train.**

      **BASE1's crate conveyor is `DOCRATES`, not PLATFORM.** The first playable base map is internally
      `BASE0`; its LevelBin handler drives Scene nodes 197, 196, 198 and 199 through four zero-speed LIFT1
      runtime objects. Generic train motion is PLATFORM and the sole disc instance is BIGGUN's three-part
      carriage. Treating the crates as a train therefore searched the wrong primitive and also discarded the
      zero-speed bindings the real handler needs.

      **Held creatures still need `monster_start_go`.** `q2_creature_world_summon` used to flip only
      `in_use`; the infantry and soldier released by BASE1's LiftRoom CREBATCH consequently had no installed
      AI think. Summoning now runs the normal monster-start path before making the record live.

      **Retail explosions have two different lights.** Spawn writes 0x40/0x40/0x40 to the model entity's
      ambient at +0x2AC. Think appends a separate C0/40/31 dynamic light whose outer radius is
      `clamp(51*(320-t),0,4096)*1300/4096` and whose inner radius is three quarters of that. The port had the
      fiery asset but fed its renderer zero ambient, producing the dark ball; both paths are now reproduced.

      **A same-map zone seam is not a player respawn.** It preserves the full movement integrator and weapon
      machine, including fire timing, and merely rebinds the viewweapon model from the new zone bank. Camera
      state is published immediately after the load. The zone object itself must be *moved*, not plainly
      copied: its chunk views point into an inline DAT archive, so a struct assignment left every pointer
      aimed into the loader's dead stack frame. Rebinding those views removed the intermittent crash and
      corrupted SortData sizes seen during transition probing.

      **The remaining small state rules are exact.** Mouse look suppresses spring-centering and clears an
      already-running recenter when enabled; a PrimaryColl cell carries the exact SortData byte offset at
      `+28`; event category bits 08/10/20 are enter/stay/leave; and BASE2 CAGELIFT1 reads its delay/wait at
      item +18/+19. Its actual
      0/0xFF pair means drop to 1491 and remain there, not return immediately.

- [x] 135. **The loading screen — there is one, #44 read half of it, and the other half is a level directory
      nobody had accounted for.**

      #44 found `0x800A3314` — `{ "LOADING", 256, 124 }`, page 46's first record — and called it *"the front
      end's loading screen, shown while `LEVELS/QFRONT/` streams in"*. It is not the front end's. It is
      **every level's**, and the function that puts it up is `0x80079178`, called from the event script's
      transition opcodes with the twelve-byte name of the level being loaded:

          8007917C  lw   v0, 0x800AEBCC          ; suppressed while this is set
          8007919C  bne  v0, zero, 0x800791F4    ;   ...return 0, no screen
          800791E0  jal  0x8006DBC0              ; the name against 0x800E465C
          800791EC  bne  v0, zero, 0x800791FC    ;   already there: return 0
          80079360  sw   0x8007901C, 0x800B2D90  ; the DEFERRED load, one shot
          80079364  jal  0x8001A384 ; li a0, 46  ; enter page 46
          80079374  jal  0x8001A474 ; li a1, 16  ; 0x800A3314, at size 16
          80079384  jal  0x8001A474 ; li a1, 16  ; 0x800A3344 — all zeros
          80079398  sh   1, 0x800C3638           ; drawable 0's +0x48: highlight

      Three things fall out that the record alone does not say. The second install is a **NULL record**, so
      nothing on the page is navigable and the selection bar — which #44's own capture shows behind START —
      is not drawn: this is a pure-text page in the same sense RESTARTING is. `0x800C3638` is drawable 0's
      `+0x48`, the highlight flag, so the word is in palette 70 rather than 68. And **the load is deferred**:
      `0x8007901C` is a one-shot hook that clears its own slot at `0x8007916C`, so the frame carrying the
      word goes out first and the read happens after it. Nothing is drawn during a synchronous read, which
      makes the console's loading screen exactly one frame held for as long as the disc takes.

      `0x800AEBCC` is the arming flag, written by the level selector at `0x8007C7C8` / `0x8007C7E8` from the
      level record's `+0x20` — "always 1 on a real level" — so the screen is on for every level and off only
      for a record that is not one.

      **And `LEVELS/QDUMMY/` is what draws behind it.** Page 46 draws over whatever scene is standing, and a
      screen meant to be up while a level is being replaced cannot borrow that level's models. QDUMMY is
      level table record 3, `Dummy`, and it holds one model — `Q2LOGO` — one named image — `FrontEnd.lbm`,
      the menu font atlas — and an 840-byte zone with no world in it. That is the letterforms this screen
      writes with and the model it turns, and nothing else; every other screen map on the disc carries
      `chars.lbm`, a world, or both. Its own `LevelBin` agrees: `module+0x96C` compares the level name
      against `"Dummy"` and installs `module+0x2E1C`, four instructions that ask for the next state once four
      frames have gone by.

      **What is NOT settled.** The `"Dummy"` string is reachable from that module and from **nothing in the
      executable** — `xrefs 0x800AC820` comes back empty — so which code path asked the console for it is
      not established here. What the directory is FOR is; a port that draws the logo from it draws the same
      pixels whatever asked for it.

      FORMATS §11.13; `src/game/loading.[ch]`; the page is in `src/menu/pages.c` where
      `q2psx-inspect menu <disc>` checks it against the executable record by record, and
      `tests/test_loading.c` pins the behaviour the tables cannot express.
