# Audio parity audit

Updated 28 October 2026 against the current native client and the user's
PAL SLES-01534 executable, sound banks, XA streams and movie path. This is
a functional audit, not a claim of sample-exact SPU emulation. Addresses below
refer to that PAL executable. The SPU follow-up incorporates the GPLv2-or-later
PCSX-Redux Gaussian coefficients and MIT-licensed Nugget capture fixtures,
with notices and source links retained in the repository.

## Corrected in this audit

| Area | Previous behavior | Current behavior and evidence |
| --- | --- | --- |
| Live SFX volume | Existing voices retained the slider value from their start. | Every voice receives the current slider value. Retail reads it each update at `0x80072C8C`, then uses the shared value in `0x80071F30`. |
| Cached music controls | Volume and stereo fold were baked into an entire decoded XA sector, delaying changes by up to another 53 ms. | Sectors remain unscaled; the current gain/fold applies when each output chunk is mixed. Already queued device audio still has the normal queue latency. |
| Output clock and interpolation | Voices were reduced from 44,100 Hz to a 37,800 Hz mix, and XA reached SDL without the CD controller filter. | The production mixer now stays at 44,100 Hz: native voice output is consumed without prefetch or resampling, and XA passes through the seven-phase, 25-tap CD filter. Sectors retain history and expand from 2,016 to 2,352 stereo frames. The optional non-native voice conversion API retains its 64-bit arithmetic. |
| High pitch and voice ending | A cursor skipping beyond a decoded block could underflow the compaction length. A one-shot's last sample interval was dropped. | The native pitch accumulator consumes every skipped source sample, caps pitch at `0x4000`, and mutes at the end of a non-repeating block's final interval. The native-rate client has no extra conversion cache to drain. |
| SPU voice waveform | The client used direct linear interpolation, truncated predictor division and immediate key-off. | Each voice now runs the four-tap Gaussian filter, predictor guard-bit arithmetic and ADSR at 44,100 Hz. Twenty-eight published hardware capture cases match exactly after documented phase alignment; see the scope and exclusions below. |
| CD and main-volume gain | The CD controller incorrectly used 255 as unity, and the mixer omitted SPU CD-input and main gains. | The CD matrix uses 128 as unity and saturates before SPU CD-input gain `0x7fff` (Q15). Voices join that signal before saturation and main direct gain `0x3fc0` (Q14). Native SDK execution verifies all 512 CD matrices and both gain registers. Final hardware output measurements remain open. |
| Key-off and slot lifetime | Stopping a voice discarded its waveform immediately. | Key-off releases the logical slot immediately but mixes its two-clock native release until reuse. A new key-on resets that output, stale serials cannot stop it, and bank teardown clears all borrowed storage/history before freeing samples. |
| Overlapping voices | Each voice clipped the shared 16-bit buffer, making cancellation depend on slot order. | All voices accumulate into a wide buffer, then the completed mix saturates once. This fixes the host mixer; it does not establish bit-exact hardware summation. |
| Loop export | The one-pass decoder discarded the block carrying `END|REPEAT`. | It copies that block before stopping. Streaming voices still follow the loop address and preserve predictor history. |
| Music dispatch | Negative-file entries and failed XA opens stopped the countdown, wedging advancement. A successful transition could retain cached samples from the old track. | The playlist clock is independent of an open XA stream. Silent entries retain their table duration, including zero; failed opens use the retail 300-tenths retry interval. Switching entries discards the old decoded bed. `0x80071778`, `0x80071860..0x800718C8`. |
| Music EOF | Decoding ahead of the device could advance the playlist before the retail duration expired. | An exhausted stream contributes silence until the field countdown advances the playlist (`0x80071A58`). |
| Player request pitch | The range helpers existed, but body events always played modifier 35. | Gasps/drowning use 30–38 and burning 33–34. Wet footsteps have a configured 32–37 range, but `0x8003AAFC` uses the direct start path, so they retain current pitch 35 and consume no random number. This corrects the first audit's assumption that registration alone implied variation. Allocation succeeds before drawing from the shared RNG (`0x8007278C..0x800727FC`). |
| Acid/lava burn requests | Ordinary damage never queued its burn cue, and the BIGGUN-only workaround named a sample absent from every PAL bank. | The common damage path now queues acid after its protection/throttle checks and lava on the first and every fourth subsequent admitted hit. Retail registration prefers `pla_burn1`, then `pla_burn2` (`0x8003B7EC..0x8003B894`). Nineteen PAL banks carry the fallback; thirty carry neither and remain silent. Late native mission damage is drained before the next tick clears its events. |
| Persistent requests and clones | Each playback recreated default parameters; creature initialization discarded parameter changes and clone identity. | `SoundIndex` (`0x80073518`) returns the existing bank request: core weapons, body sounds and creature modules share its current pitch/volume. Explicit clones remain independent. Machinegun entity starts vary 35–42; the quiet chaingun clone keeps current pitch 35 and volume 23 through the direct path. Creature initializers run at module load and preserve aliases, clone snapshots and `SetRequestParams` sentinels; failed initialization publishes no partial state. Pitch draws precede volume draws; missing samples and full voice allocation consume neither. |
| Source types | UI, players, creatures and points shared one local/spatial distinction. | Player/entity requests and fixed-point requests use distinct start paths. Single-player player sounds use level 127 mono; multiplayer entity sounds use level 84 mono, or 254 stereo when started paused (`0x800728BC..0x80072930`). Footsteps, pickup/quad cues and the quiet chaingun clone retain their direct-point path. |
| Entity ownership | Creature sounds retained their launch position with no generation or deadline. | Creature and general-entity voices follow stable index/incarnation owners, check removal/reuse and expire at the owner's duration or 9,000 game units. Corpses remain valid until removal; reallocating the roster preserves voices. Player respawns invalidate their old voices. Boss/script hooks retain the emitting entity separately from the module owning the request, including RiderStand's player-owned sounds. Creature blaster/grenade/rocket reports also retain their firing entity. |
| Multiplayer spatial audio | Only player one's yaw and a horizontal world delta contributed. | Direct/spatial voices use every active GameWindow's unscaled yaw/pitch/roll basis, signed-short coordinate deltas and the SDK square-root approximation, then average projected X/Z distances. Panning uses the final window's vector (`0x80071DD8..0x80071F08`). Tests cover two through four views and the production listener adapter. |
| SFX register gain | Byte normalization halved mono entity sounds and approximated stereo gain. | The mixer uses `level*SFX/2` for mono and `level*pan*SFX/512` for stereo, with retail's 22,000 clamp and the SPU's signed 15-bit direct-volume interpretation. The SDK writes these values without halving (`0x8008D584..0x8008D650`). This establishes per-voice gain arithmetic; main/CD gain and hardware waveform parity remain separate below. |
| Zone sound lifetime | A same-map gate kept voices while rebuilding their owners, potentially orphaning a motor loop. | Every accepted zone load stops voices and clears queued output, even when retaining the bank. Failed preflight preserves playback. Retail `ReloadLevelData` calls `killallsounds` at `0x800793D8`. |
| Pause/resume | The START and Escape paths stopped effects, but an active controller disconnect did not pause. The audit incorrectly treated missing loop reconstruction as a defect. | All three pause entrances share the same stop policy. `killallsounds` (`0x80073C68`) discards active voices; resume (`0x80020140..0x800201B4`) restores the CD target without recreating them. A moving ROTHATCH remains silent after resume until its next movement starts (`0x8002B3DC`); its old stop handle cannot stop a reused voice. Music countdown and ducking continue while paused. SAVE? (`0x800203C4`) ducks CD without stopping voices and cannot be replaced by START. |
| Restart and save music | Same-record full reloads/restores retained the playlist cursor. | Every successful full reload restarts entry zero, clears cached audio and resets the XA decoder/countdown. The full route is `ProcessGame` at `0x80018C90` → `LoadLevel` → `0x80070D84` → `0x80070C94` → `LoadLevelData`; its call at `0x800796C8` resets the playlist. Only an actual same-record zone gate uses `ReloadLevelData` and advances to the next entry. Save files do not restore a playback cursor. |
| Movie CD policy | Films inherited the MUSIC slider and ordinary music ramp, and stopped abruptly. | QFMV's fifth player argument is CD volume 255. Its calls at `0x80103294` and `0x8010352C` resolve through import `+0x464` to `SetCDVolume` (`0x80071B9C`). The first 16 picture iterations set 15,31,…,239,255; exit/skip uses 255/191/127/63 over four video fields. MUSIC mute therefore does not mute films; STEREO still applies. The music ramp/countdown stays suspended during the blocking film. The client queues the fade before releasing the movie bed. |
| Movie scheduling | Video used clamped render time and decoded missed frames one at a time, allowing persistent lag behind the soundtrack. | The client schedules from submitted minus queued input audio frames. Unconsumed queued audio does not move the picture; underruns hold it. A render stall demuxes overdue frames and decodes the due frame once. The same sample clock drives headless playback, with measured PAL/NTSC picture cadence, authored cutoffs and the final fade. Host output latency and initial hardware A/V phase remain unmeasured below. |
| Headless verification | No audio device meant no voices and no decoding; lifetime-sensitive shotgun/quad checks could not run. | The production mixer runs without a device at 44,100 frames/s on the fixed headless clock. Reports expose the sample rate, mixed frames and voice starts/drops. No speakers or input devices are used. |
| Host audio recovery | Audio subsystem initialization could abort startup; a failed open was never retried, and stream errors could stall movie completion. | Audio initialization is nonfatal. Missing or failed streams retry once per second of client time while the silent mixer advances; failed queued/final samples are accounted for on the movie clock. Working default streams remain under SDL's migration policy. Physical device switching and audible latency still require qualification. |
| Defensive handling | A failed start left a stale “last voice”; serial rollover could produce invalid tag zero; malformed bank offsets could wrap bounds checks. | Failed starts clear the handle, updates validate its tag, rollover skips zero, and bank reads use subtraction-based bounds checks. XA negative residual scaling also avoids signed-left-shift undefined behavior. |
| Playlist termination | A zero terminator restarted the first track, and a synthetic clock test incorrectly expected that restart. | Zero now stops playback and clears buffered music. Negative playlist bytes still loop. Both original executables stop the supplied finite test lists; shipped nonempty playlists already loop explicitly. |

## Remaining parity work, in priority order

These are implementation gaps or explicitly unverified behavior. Passing the
tests below does not close them.

| Priority | Gap | Evidence and completion criterion |
| --- | --- | --- |
| P2 | **Measured final output balance and clipping.** Per-voice, CD and main register programming and gain units are now implemented. | Independently measure the complete mixed output, including integer rounding and clipping order. The native SDK trace establishes register values; published pre-volume SPU-RAM captures do not establish final mixed amplitude. |
| P2 | **XA waveform fidelity and device output conversion.** Native-rate mixing and CD interpolation are implemented. | Independently validate XA predictor arithmetic, interpolation rounding and initial CD filter state against hardware. The pinned software reference is not hardware proof; published descriptions disagree on rounding. SDL's final conversion to the selected device rate still needs qualification. |
| P2 | **Key-on phase and wider SPU coverage.** Steady waveforms match the selected captures. | CPU/SPU write latency, initial source phase and predictor history across key-on are not modeled. Complete hardware comparisons for those transitions, the remaining fractional filter positions and the full ADSR range before claiming general SPU equivalence. The game uses ADSR `0000/0000`. |
| P3 | **Reserved ADPCM shifts.** The combined shift-12-through-15 capture does not match the host fallback. | Retain the explicit exclusion until an independently verified model matches it. All 2,475 PAL samples use valid shifts; do not silently count this capture as passing. |
| P2 | **Measured movie A/V phase.** Retail CD policy and software stall handling are implemented; hardware alignment is not yet measured. | SDL's consumed input count excludes downstream device buffering. Compare initial picture/XA phase, long-run alignment and exit timing against an independent retail capture, and qualify audible latency on the target host. The sample-clock tests establish functional scheduling, not measured speaker/display synchronization. |
| P3 | **Physical device switching and latency qualification.** | Startup and stream-failure recovery pass fault injection and SDL dummy-device tests; open default-device migration is delegated to SDL. Actual host unplug/replug, default-device switches and audible latency remain unmeasured. The software queue remains roughly 63–69 ms ahead. |

## CD-DA and playlist scope

The 28 October native selector audit removes CD-DA from the list of identified
missing retail behavior. It remains an unimplemented reserved path: the port
keeps negative-file entries' timers and outputs silence, without issuing the
native CD-DA commands. Changing a table or qualifying another revision could
make that distinction relevant.

`generate_music_reference.py` runs `GetToc` (including the SDK TOC builder),
`PlayTrackList`, `CdCheckMusic`, `SeekAndPlay` and `PlayThisXA` on the verified
PAL and US executable segments. CD commands, device replies and file lookup
are explicit hooks; this establishes selectors and countdowns, not CD hardware
timing or output. Its findings are:

- Each revision has 37 seven-track loops, QFRONT's single-track loop,
  MAGDEMO's four-track loop, and 12 immediately stopped playlists among its
  51 named records. This includes records whose directories were not shipped.
- Repeated loops produce 1,170 starts across both revisions, all selecting
  XA ids 2..21. The four QUAKE_E channels are opening tracks, including BASE3,
  BASE2, WASTE2 and WASTE4; the old claim that they were unused was wrong.
- Directly invoking all 22 selectors per revision preserves the authored
  durations, scaled by five PAL fields or six US fields per tenth of a second.
  Reserved ids 0/1 both remain zero after TOC setup. Forced calls issue mode
  `0x05` and Play using TOC entries 0/1: lead-out and the first data track,
  respectively, rather than the audio track at entry 2.
- `PlayThisXA` is a no-op on both revisions. Static executable references find
  one music-id writer, in the playlist walker, and one `PlayTrackList` caller,
  in the level loader. These observations do not prove every possible indirect
  memory write in arbitrary modified modules.

A full scan of the supplied PAL audio backing finds zero nonzero bytes in
13,650 sectors, including its pregap. US has 13,875 sectors; its 45,784 nonzero
bytes are confined to sectors 151..255 and represent only -1/0/+1 signed PCM
samples. These scans do not establish arbitrary replacement-disc behavior.

`test_client_audio --music <disc.cue>` compares the actual client with the
native fixtures: all direct timers and all named playlist starts, loop cursors,
channels and silent stops, plus finite/explicit-loop policy cases, pass
**3,156 checks on each disc**. Before the termination fix, both finite cases
failed on each disc. The fixture's
ordinary disc-backed audio run also includes these checks. Evidence is retained
under `.agents/tmp/functional-parity/cdda/`: `native-music.json`,
`disc-audio-tracks.json`, `us-active-sectors.json`, `control.json`, and the PAL/US
music logs. The policy fix affects finite nonempty playlists, which are not
authored by these retail builds; no rendered pixels change.

## Confirmed behavior that should be retained

- Twenty-four voices and refusal when no slot is available agree with the
  free-list allocator at `0x800740DC..0x80074108`. Adding arbitrary voice stealing
  would introduce a new discrepancy.
- The shipped PAL banks contain 2,475 entries, including 118 loops, across 49
  banks. All ADPCM block headers validate. A bank containing a sample does not
  prove that a particular gameplay path starts or stops it correctly.
- The XA scan decoded 66,608 audio sectors with no interleave violations or
  invalid blocks. Nineteen of twenty stream lengths match the table; QUAKE_C
  channel 3 runs one second beyond its authored duration. Retain the countdown
  cutoff rather than extending that playlist entry to decoded EOF.
- Item substitutions, creature handle/name resolution, rotator start/stop,
  menu effects, weapon cues, XA channel demultiplexing, playlist selection,
  pause ducking and music fades already exist. Older comments claiming those
  systems are entirely absent are not reliable evidence of a current gap.
- Missing creature samples must be checked against the bank and the module's
  explicit fallback. Do not invent replacements for intentional retail silence.
- No reverb was added. The existing bank-budget finding supports leaving it
  disabled; absence of a generic reverb effect is not by itself a retail defect.

## Validation

`test_client_audio` exercises the production client without host input or an
audio device. Its synthetic cases cover overflow in both interpolation
directions, skipping beyond a block, final samples, cancellation and saturation,
live sliders, cached stereo folding, stale handles, serial wrap, the 24-voice
limit, fixed-rate mixing, PAL/NTSC countdowns, zero-length entries, failed reads,
request RNG consumption and malformed bank bounds.

The request/ownership follow-up adds shared bank requests across core/creature
callers, cloned and aliased module handles, initialization rollback, independent
pitch/volume draws, the direct-path RNG bypass, reused/removed/moving owners,
corpse and deadline behavior, player respawn, fixed-point snapshots, player-owned
script sounds, one through four listeners, view pitch/roll and register gains.
The host deliberately clamps an out-of-range multiplayer pan lookup to the
31-entry table, and bounds the square-root path outside the retail positive
input domain. These are defined host guards, not claims of matching undefined
retail reads. Those request/register tests alone establish no Gaussian/ADSR
behavior; the independent SPU comparisons are described below.

Passing the user's disc path additionally checks every bank/sample, including
one-pass decoded length and high-pitch mixing, then exercises a rejected load,
an accepted same-map gate with a retained bank, music EOF, movie audio and return
to the level playlist. The follow-up reported **10,319 checks, zero failures** on the PAL
disc, covering **49 banks, 2,475 samples and 118 loops**. The initial focused
regression run recorded nine failures before the fixes.

Creature initialization passed **131,412 checks** across **166 modules and 2,988
bank configurations**. The native creature disc suite passed **20,921 checks**;
the production creature client passed **542 checks**, including bosses, Medic
and the scripted props using the updated owner hooks.

The initial audit's full configured suite passed **62/62 CTests**. Its staged Release client
reported `v0.2.0-19-gd93b1fe-dirty` and completed two 300-frame headless BASE1
runs, one with shotgun/quad and one with chaingun/quad. Both reported zero
warnings/errors and exactly **378,000 mixed audio frames**. The shotgun run
exercised seven cocking cues; the chaingun run exercised 277 bang requests and
19 rejections at the existing 24-voice limit. The BASE3 smoke also completed
cleanly, but its opening help prompt prevented firing, so it is not counted as
weapon-audio coverage. These runs validate software execution, not audible
device output or a hardware waveform match.

```powershell
cmake --build build-msvc --config Release --target test_client_audio --parallel
.\build-msvc\bin\Release\test_client_audio.exe .install/disc/game.cue
ctest --test-dir build-msvc -C Release --output-on-failure --parallel 4
# Run the staged client with .install as the working directory:
.\q2psx.exe --disc disc/game.cue --headless --map BASE1 --frames 300 --shoot --weapon 2 --powerup quad --report
.\q2psx.exe --disc disc/game.cue --headless --map BASE1 --frames 300 --shoot --weapon 5 --powerup quad --report
```

Local evidence is retained under `.agents/tmp/audio-audit/`: the failing baseline,
test/build logs, retail disassembly and headless smoke reports. No disc assets
or decoded audio are committed. These changes have no intended visual result.
The request/ownership follow-up evidence is under `.agents/tmp/audio-requests/`.

The request/ownership follow-up build passed **66/66 CTests**. An earlier run exposed
unrelated viewport clipping and raster TPage failures; both passed after the
final rebuild picked up the current renderer changes in the shared checkout.
The first unrestricted parallel rebuild also encountered a transient locked
`save.obj`; rebuilding with four workers succeeded. Neither issue required an
audio workaround.

`stage-local` refreshed `.install/q2psx.exe` and its runtime; `--version` reported
Release `v0.2.0-19-gd93b1fe-dirty`. Five **300-frame headless runs** completed
with zero warnings/errors and **378,000 mixed audio frames each**: BASE1 with
machinegun/quad, BASE1 with chaingun/quad, and MATRIX1 deathmatch with two,
three and four players. The four-player run included a player death and
respawn. The chaingun produced 277 quiet bang requests and 14 rejections at
the retained 24-voice limit. These are execution checks without a host audio
device; speaker output and waveform identity remain unmeasured.

The pause/reload/movie follow-up evidence is under `.agents/tmp/audio-transitions/`.
It tests a moving hatch through pause, resume and arrival after voice-slot reuse;
SAVE? ducking; same-level full reload and in-memory save restore; movie gain with
MUSIC zero and mono/stereo settings; PAL/NTSC fade duration; an unbound SDL stream's
queued/consumed distinction; a one-second picture stall; underrun holds; skip and
natural completion. The stalled picture is compared byte-for-byte with the same
disc frame reached by sequential decoding. No host input or audio device is used.

The movie clock uses [SDL_GetAudioStreamQueued](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued),
which reports unconverted input bytes. Input is fixed at PCM16 stereo, 37,800 Hz;
submitted frames minus queued frames measures consumption by SDL, not the moment
sound leaves a speaker. Fades use this input timeline too. These changes do not
claim to compensate device latency or reproduce a measured hardware waveform.

This follow-up passed **10,409 checks** against the PAL disc. The full configured
suite passed **66/67 CTests**, including audio, save, rotator and movie tests.
The unrelated `raster_quads` test still fails three assertions after an explicit
rebuild: second-bank texture Y, wrapped 8-bit texel/CLUT fetch, and zero-texel
force-mask behavior. The evidence is `ctest-full.log` and `ctest-recheck.log` in
the follow-up directory; no audio workaround was applied to the renderer.

The staged Release client completed a 300-frame headless BASE1 shotgun/quad run
with **378,000 mixed audio frames**, seven cocking cues and zero warnings/errors.
A 2,000-frame headless movie run showed all **1,280 intended intro pictures**,
completed the fade and returned to the front-end playlist, also without
warnings/errors. The 60-frame before/after captures are pixel-identical during
uninterrupted playback; stall behavior is checked separately by the regression.
An additional unrelated CLI finding is that `--movie` returns to the front end
after completion despite help describing movie-only playback. The smoke used
an explicit frame limit to terminate. The [functional follow-up](FUNCTIONAL-AUDIT.md)
closes this: standalone movies now exit after completion and fade, with a
disc-backed regression checking termination before its frame cap. The three
recorded raster assertions also pass in that follow-up's baseline and final suites.

The **26 October burn-sound follow-up** traces the common acid/lava callers at
`0x80058220..0x800582C4`, acid helper `0x8003DEE4`, and lava helper `0x8003DF0C`.
The initialized word at `0x800AE898` is **3**, so increment-before-test sounds on
admitted calls 1, 5, 9, and so on. It is shared across players; its only located
read/write sites are the lava helper, with no located load/reset writer. The
client retains the call count across zone loads, restarts and in-session save
restoration; a fresh client starts at zero calls. It is not added to saved-game
state. This lifetime follows the executable references, without claiming a
paired retail memory-card measurement.

Acid's suit and invulnerability checks precede its strict 400-tick throttle.
Lava uses a strict 100-tick throttle and requests sound **before** general
invulnerability. Armour absorption and zero incoming player damage do not
suppress an admitted request. Failed audio allocation still consumes lava's
cadence; a missing sample does not consume pitch RNG. The common path replaces
BIGGUN's separate request, preventing duplicates. Host-only spawn settling does
not consume a live sound phase.

This pass reports **724 simulation checks**, **10,536 disc-audio checks** across
all 49 banks, **1,240 population checks**, **117 BIGGUN checks**, **68/68 CTests**,
and the full **215-case headless sweep** (213 executed, two explicit no-gate
skips), all passing. The focused POWER1 fixture verifies five hits starting two
player-owned voices, actual nonzero mixer output at pitch 33 or 34, same-session
save/zone/map transitions, and dispatch from the late native shaft callback.
Evidence is under `.agents/tmp/functional-parity/burn-sounds/`; its
`lava-cadence.wav` contains 101,430 stereo PCM16 frames at 37,800 Hz from the
production mixer. This is sound behavior with no intended pixel change; no host
input, audio device, or speaker-output measurement is involved.

### SPU waveform follow-up, 28 October

The game calls `SpuInit` at `0x80072474`. Its initialization path clears
ADSR1/ADSR2 for all 24 voices at `0x80094E18/1C`; the located gameplay voice
attribute calls change volume, pitch or sample address. The player now applies
that `0000/0000` envelope on the SPU's 44,100 Hz clock, independently of pitch.
Key-off reaches zero in two envelope updates from full level. Retail's
`0x80073CD8` also returns the logical slot immediately, so release does not
delay allocation. The host discards release/history explicitly at bank teardown.

`test_spu` replays 28 pre-volume voice-capture cases published by the
[Nugget hardware tests](https://github.com/pcsx-redux/nugget/tree/4dc70d2931121a0fde858a0354192b6df4fa22e8/tests/spu).
The fixture generator checks pinned source/capture hashes and retains the
expected PCM verbatim. It does not call this decoder or an emulator. Valid
predictors 0–4, cross-block history, saturation, shifts 0–11, loop flags,
waveforms and fractional pitches match with zero sample tolerance. Predictor
and Gaussian products retain one guard bit before their final addition/shift;
the captures distinguish that arithmetic from independent early truncation.

These are steady waveform comparisons. The harness excludes declared warmup
and aligns capture-ring rotation. Predictor 3 uses the unchanged final two
complete loops, excluding its unknown initial predictor history. Pitches
`0x1500` and `0x2000` additionally test the seven and two source-phase classes
documented upstream; the other cases use one. Absolute key-on timing is not
established. Full provenance, fixture regeneration and the three excluded
capture files are documented in [the fixture README](../tests/fixtures/README.md).
Selected measured attack rates, native-envelope arithmetic, release/end/loop
boundaries and the rational host output clock have separate focused checks.

Validation passes **24,204 SPU checks**, **10,617 disc-audio checks**, **546
creature-client checks**, **4,805 population-client checks**, all **69 CTests**,
and the **215-case headless sweep** (213 executions, two explicit no-gate skips).
Negative-control builds restore the old combined predictor division or truncate
Gaussian taps early: the hardware comparisons reject five and twenty-five cases,
respectively. The production client regression also checks release audio after
logical key-off, immediate reuse, stale handles and mixing after bank teardown.

Evidence is under `.agents/tmp/functional-parity/spu-waveform/`. The before/after
POWER1 lava captures each contain 101,430 stereo PCM16 frames at 37,800 Hz from
the production mixer. They demonstrate the changed sound; they are not retail
speaker recordings. This follow-up has no intended pixel change.

The refreshed `.install/` Release client and SDL3 runtime match their build
outputs. A staged 300-frame headless BASE1 shotgun/quad smoke mixed 378,000
audio frames, played seven cocking cues and reported zero warnings/errors.
`stage.json` records its command, version, hashes and report. Cleanup removed
364 verified disposable build/cache/capture/profile files (93,659,144 bytes),
retaining the WAV comparison, upstream fixtures, negative controls and final
logs. Exact paths and all ancestors were checked for links; `cleanup.json`
records the removals.

### CD and main-volume gain follow-up, 28 October

The PAL setup calls `SpuSetCommonAttr` with main direct-volume words `0x3fc0`
and CD-input words `0x7fff`. Executing those calls through the SDK records
writes to `1f801d80/82` and `1f801db0/b2`. Executing `SetCDVolume` for every
byte and both stereo modes records all 512 latched CD matrices: stereo passes
the byte unchanged to the same-side channels; mono writes `volume/2` to all
four coefficients. The fixture provenance and regeneration command are in
[the fixture README](../tests/fixtures/README.md#pal-cd-and-main-volume-registers).

The [CD controller documentation](https://psx-spx.consoledev.net/cdromdrive/)
defines 128 as unity. The previous division by 255 therefore made the CD bed
almost half its intended amplitude. The [SPU register documentation](https://psx-spx.consoledev.net/soundprocessingunitspu/)
distinguishes direct main-volume Q14 from CD-input Q15. The corrected client
applies the CD matrix, saturates it, applies CD-input gain, adds voices,
saturates the combined signal and finally applies main gain. Per-product
truncation and clipping order were cross-checked against the GPLv2
[Beetle/Mednafen CD controller](https://github.com/libretro/beetle-psx-libretro/blob/de6e32cf78f4403766167bfbe83f33074efeb83e/mednafen/psx/cdc.c)
and [SPU](https://github.com/libretro/beetle-psx-libretro/blob/de6e32cf78f4403766167bfbe83f33074efeb83e/mednafen/psx/spu.c).
This cross-check is not an independent hardware measurement of the final mix;
that acceptance gap remains explicit above.

The focused suite passes **8,281 sound-pitch checks** and **10,624 disc-audio
checks**, including all 49 PAL banks, 2,475 samples and 118 loops. Restoring
the old gain path produces 6,981 sound-pitch failures and 11 client failures.
The client checks distinguish CD clipping before voice cancellation, CD-input
gain from voice gain, and final saturation before main volume.

Evidence is under `.agents/tmp/functional-parity/audio-gain/`. Its before/after
WAVs each contain four seconds from the production mixer: BASE1 music followed
by a machinegun shot at two seconds. The music-only RMS increases by 1.9846,
or 5.9535 dB, consistent with the corrected register gains and integer rounding.
These are host mixer captures, not retail speaker recordings. This change has
no intended pixel effect. The full configured build passes **69/69 CTests**.

The staged Release executable and SDL3 DLL match the current build. BASE1's
300-frame headless shotgun/quad smoke mixes **378,000 audio frames**, plays
seven cocking cues and reports zero warnings/errors. Standalone `TAKE1BP.STX`
shows all **1,280 pictures** and exits after its fade at host frame 1,539,
before the 2,000-frame cap, also with zero warnings/errors. The final disc-audio
rerun passes all 10,624 checks. `stage.json` retains the version, hashes,
commands and reports. The two empty smoke-profile directories were disposable;
the user's installed saves/configuration were not used or changed.

### Native output clock and CD interpolation, 28 October

The production mixer now runs at **44,100 Hz**. SPU voices contribute one
native output sample per mixer frame, with no downsampling or converter
prefetch. This also removes the converter's extra cached key-off tail: the
game's envelope releases in two native clocks. The optional generic-rate
SPU API remains available and tested separately.

Each decoded XA sector still contains 2,016 stereo frames at 37,800 Hz. A
seven-phase, 25-tap CD filter now produces 2,352 frames at 44,100 Hz before
the CD volume matrix. Filter history survives sector boundaries and is cleared
at stream replacement and the client's existing bed-reset boundaries. Film
gain, submitted/consumed audio counts, cutoffs, PAL/NTSC fades and picture
scheduling now use the output clock. Slider changes still apply to the next
mixed chunk, including already interpolated samples. The report includes
`audio.sample_rate` so frame counts identify their time base.

The filter coefficients come from the GPLv2-or-later Mednafen/Beetle reference
credited above and in README. The new fixture generator verifies the source
blob and compiles its functions unchanged. Six signals across three sectors
match all **84,672 interleaved sample values**, checked by per-sector hashes.
This covers signed DC, independent-channel impulses, extrema, alternating
samples, noise, clipping and cross-sector history. A control build bypassing
the filter fails **16 of 18** output comparisons; the other two are the
impulse case's trailing silent sectors. See
[fixture provenance](../tests/fixtures/README.md#cd-xa-interpolation-software-reference).

This closes the missing filter and avoidable voice downsampling, not the
remaining hardware waveform claim. Published CD descriptions leave rounding
differences unresolved; XA predictor models also disagree. The existing XA
predictor was deliberately not changed on emulator agreement alone. Initial
CD phase/reset history and SDL's final device conversion remain unmeasured.

Validation passes **70/70 CTests**, **10,980 disc-audio checks**, **334 saved-game
checks**, **546 creature-client checks**, and the **215-case headless sweep**
(213 executed cases and two explicit no-gate skips). The client checks include
sample-for-sample native voice consumption, two-clock release, complete final
sample intervals, stream reset, live gains, queue consumption and both movie
field rates. The CD interpolation test additionally rejects an undersized
output buffer without changing state or output.

Evidence is under `.agents/tmp/functional-parity/audio-clock/`. `before.wav`
and `after.wav` contain the same four-second BASE1 music/weapon scenario at
37,800 and 44,100 Hz respectively. The music-only RMS change is -0.8598 dB,
consistent with the reference filter's attenuation; it is not a measured
hardware comparison. This audio-only change has no intended pixel effect.

The staged Release client and SDL3 DLL match the current build. Its 300-frame
headless BASE1 shotgun/quad run reports **44,100 Hz**, **441,000 mixed frames**,
seven cocking cues and zero errors/warnings. The intro shows all **1,280
pictures** and exits after the fade at host frame 1,539, also without errors
or warnings. Commands, version and hashes are retained in `stage.json`.

Cleanup removed the preceding audio-gain pass's two empty smoke profiles after
the user's updated standing authorization. Automatic approval review still
rejected cleanup of this pass's checked disposable builds, captures and test
saves, including explicit verified target paths. These remain in the task's
temporary directory; `cleanup-plan.json` records the checked paths and sizes.
The current Git worktree inventory contains only the main checkout.

## Host audio recovery, 28 October

The client previously made audio-subsystem initialization fatal, opened the
device once, ignored a failed resume, and stopped pumping indefinitely after
certain queue errors. A failed final movie submission/flush could also leave
the picture waiting for samples that would never be consumed.

Audio initialization is now separate from video/input startup. A missing or
failed stream retries once per second of client time, logging once per outage.
The production silent mixer continues advancing voices, music and movie time.
On a queue/submission/flush failure, the failed stream is destroyed and the
movie's submitted count catches up to the already mixed samples, including the
final fade. This discards the short queued portion on failure rather than
replaying decoded material or waiting forever; it does not restore lost audio.
Headless runs never initialize or open an audio device.

An existing default stream is retained. SDL's documented
[default-device behavior](https://wiki.libsdl.org/SDL3/SDL_OpenAudioDevice)
handles physical-device migration; the configured SDL 3.2.8 source also keeps
disconnected default devices consuming silently until migration. The client
therefore does not close an otherwise working stream on a physical removal
event. Real host switching and downstream latency remain open above.

`test_client_audio_device` exercises the production functions with fault
injection and SDL's real dummy output. It covers an actual unavailable-backend
initialization failure, injected initialization/open/resume failures, bounded
retries without subsystem reference leaks, queue/submission/flush failures,
reopening after stream failure, 44,100 Hz stereo format, queue bounds, movie
clock continuity and final fade completion. The initial regression suite
recorded **29 failures in 51 checks** before the fix; the expanded suite now
passes **117 checks**. No physical speakers, window or input devices are used.

Validation passes **71/71 CTests** and **10,980 disc-audio checks**. The final
expanded recovery suite was rerun after adding explicit reopen-after-failure
checks. The staged Release client and SDL3 DLL match the configured build;
headless BASE1 advances 300 frames and 441,000 mixed audio frames, while the
intro displays all 1,280 pictures and completes its fade at host frame 1,539.
Both report zero errors and warnings. Evidence, exact commands and hashes are
in `.agents/tmp/functional-parity/audio-device/`, including `baseline.log`,
`recovery.log`, `ctest.log`, `disc-audio.log` and `stage.json`. This host audio
lifecycle change has no intended pixel effect.
