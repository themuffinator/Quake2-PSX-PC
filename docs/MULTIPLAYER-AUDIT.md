# Multiplayer parity audit

The playable modes are Deathmatch, Team Deathmatch and Versus. Evidence below
comes from the user's PAL disc, decoded with the repository's inspector; no
external implementation or copyrighted game assets are added to the repository.

## Corrected behaviour

| Area | Retail evidence and resulting behaviour |
| --- | --- |
| Default teams | `0x8001BED4..0x8001BF04` sets team to `player & 1`, separately from personal colour. Four-player Team Deathmatch starts blue/red/blue/red; teammate damage is scored as a team kill. |
| Player setup | PROCEED now opens QFRONT `+0x59CC`'s independent player panels. Players edit their eight-character name, personal colour/team and controller profile, then select START. The match waits for every READY; a one-team game cannot start, and disconnected pads retain their state for reconnection. |
| Configured identities | Match startup preserves the selected teams and colours. Custom names reach winner banners and results. Settings cards store all four identities and controller profiles, with defaults when reading older cards. |
| Player bodies | `0x8003B2B0`'s mode dispatch and table at `0x800AC630` choose team colour in modes 1, 2 and 4, personal colour in 0, 3 and 5. The body mesh now uses that colour instead of the viewport index. |
| Respawn selection | Occupancy follows active entities with positive health, rather than waiting for a death animation to leave its alive stage. |
| End banners | QMULTI `+0x0F24..+0x1114` displays the size-16 winner at (256,20) and size-32 result at (256,124), with highlighted text and colour bands. Players cannot move, fire, change weapons or pause over the banner; the world and countdown continue. |
| Versus limits | Versus forces an unlimited match clock even when the CLI supplies a time limit. Round limits remain effective. |
| Score ordering | QMRESULT `+0x0D64` sorts scores descending and preserves tied order. Mode 1 sorts teams by total, then groups their sorted members. Represented zero-score teams remain visible. Versus displays and sorts round wins. |
| Results presentation | QMRESULT `+0x0E1C` supplies row spacing and fixed-width columns; `+0x1714` supplies title and prompt positions. The client loads the actual results map with one viewport and the shared menu font. |
| Results artwork | QMRESULT `+0x1890/+0x2508` draws `wipteam1.lbm` as two 8bpp sprites at half brightness, using its palette at `0x3FC0`. The renderer follows that path with the disc asset. |
| READY ownership | READY stays attached to player ID after sorting. A controller release and fresh press between render frames is retained; a button held on entry is ignored. |
| Return destination | Request 18 calls `0x8007CCE0(2)`; QFRONT `+0x3258` selects builder `+0xCF68`, the MULTIPLAYER mode menu. The port previously went directly to match setup. |

## Preserved original behaviour

The Team Deathmatch **winner banner** at QMULTI `+0x0660` marks participating
teams but reads `frags[team]`, not `team_frags[team]`. This can disagree with
the correctly totalled results table. It is an original quirk, retained for
parity rather than silently changed. The kill matrix's diagonal-only writes,
strict countdown expiry and equality check on frag limits are also retained.

CTF, Tag and Team Tag are cut modes: the shipped menu cannot select them and
the disc lacks the flag gameplay needed to complete them. They are not treated
as missing features of the three shipped modes.

## Verification

- `test_multiplayer`: alternating teams, teammate/enemy scoring, stable player
  and team sorting, negative scores, zero-score team inclusion, bounded output,
  round-win scores, fixed columns and READY ownership.
- `test_client_split`: colour selection, neutral input through the end banner,
  live countdown, short READY presses and Versus clock override, alongside the
  existing independent-controller, hotplug, pickup, combat and respawn checks.
- `test_client_split <disc.cue> [capture.ppm]`: loads MATRIX5, transitions to
  QMRESULT with four players, preserves scores and controller styles, verifies
  bright text and background pixels, checks repeated loads preserve readiness,
  and returns to the MULTIPLAYER page. All rendering is headless.
- `check_split_support.py`: all thirteen arenas with four live players, complete
  two-player matches in all three shipped modes, and four-player Deathmatch and
  Team Deathmatch through all-player READY and the return to the menu.
- `test_client_menu --headless <disc.cue> <existing-output-dir> --players`:
  enters through PROCEED, edits four names, teams and controller profiles using
  independent SDL-local virtual pads, checks rapid presses, waits for every
  READY, loads the arena and its results, and returns to the menu. It also
  captures and starts two-player Deathmatch and three-player Versus.

## Remaining limits

The [functional follow-up](FUNCTIONAL-AUDIT.md) gives each pausing player an
independent controller profile, including style, inversion, vibration and slider
values, persisted in settings cards and saved games. It also documents the newly
restored player setup and its native viewport calls. Keyboard/mouse and controller-slot
assignment are host adapters. Physical controller ergonomics and rumble need a
hands-on session; automated tests use synthetic SDL controller events without
reading or controlling the user's input devices.

Earlier notes describing unsorted rows over a frozen arena in
`openquestions.md` are historical; this audit and FORMATS §16.11 supersede them.
