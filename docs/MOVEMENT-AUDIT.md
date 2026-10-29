# Player movement audit

The reference for this pass is the retail PAL executable on `SLES-01534`.
Its player movement is distinct from PC Quake II's `pmove`. The audit compared
the live think (`0x8003A1C8`), shared movement (`0x80039AA4`), collision mover
(`0x8004583C`) and body separation (`0x80051258`) with the port's implementation.

The inspected live-player branches already implement the console's clamped
approach, wish rotation, posted jump impulse, look integration, swimming,
ground projection and fall response. The confirmed gaps were in the movement
after the live think is replaced on death.

## Corrected death movement

| Retail handler | Movement behavior |
| --- | --- |
| Live player, `0x8003A1C8` | Controls, view adjustment and volume contacts, followed by shared physics with body separation |
| Corpse, `0x80039550` | Gib check first; approach all three velocity axes toward zero by `dt * 40`, then shared physics; body separation stops once the handler raises `DEAD` |
| Down body, `0x8003E238` | Dissolve gate, then gib check if the gate refused; shared physics without braking or body separation |
| Fade/dissolve/gibbed/gone | No player movement |

The old simulation fed a neutral pad through the entire live-player think.
This stopped intentional walking but still changed the view, environment flags
and volume contacts. It also missed vertical corpse braking and let the fly
setting overwrite corpse momentum. The independent death helper used `dt * 5`:
the instruction sequence at `0x800395A0..0x800395B8` also shifts left 19 and right
16, giving the missing factor of eight.

`player_control` now contains the live think's controls and environment work;
`player_move` contains the shared physics. The client derives a movement mode
from each player's death stage before movement. This includes parked
split-screen players and refreshes the mode after another player's frame may
have damaged a body. The first dissolve-transition tick still moves because
the retail gate skips the gib check and joins the mover's tail. Later dissolve
ticks do not move.

The mover tests the death chain's `DEAD` bit at `0x8004597C`, which is raised
after the first single-player corpse move or after the deathmatch animation
finishes. Its initial moves still separate from other bodies. The simulation's
earlier input-blocking copy of `DEAD` must not shorten this period.

An animating corpse remains solid to other movers. The down-body transition
sets retail flag `0x8000`, which body separation rejects, so down and later
stages no longer block players. Respawn restores live movement. The movement
fields are derived runtime state and do not change the saved-game format.

Dead players no longer activate trigger volumes. The event runtime still
executes queued scripts so world behavior such as an `ALWAYS` hook continues.

## Validation

`movement_client` exercises the actual client/simulation boundary. It checks:

- All-axis braking and subsequent gravity at `dt` 1, 6, 10, 12, 20 and 30.
- Frozen corpse view/controls, retained momentum with the fly setting, and the
  previous-frame displacement used by the mover.
- All four player slots, corpse/body/frozen stages, solidity, gib and dissolve
  ordering, respawn, and a world clock that advances once per shared tick.
- Suppressed dead-player volume contacts with continued queued-script execution.

The optional disc run uses BASE0's real collision and renderer, raises the
player 600 units above the spawn floor, gives it downward velocity 2000 and
advances eight headless frames at a fixed 1/30 second. Before the fix it reached
feet Y -687 with velocity Y 4560; afterward it reached Y -987 with velocity Y
1360. The framebuffer is written directly by the engine.
The same disc test also checks separation between overlapping players before
and after the death chain raises `DEAD`.

```powershell
cmake --build build-msvc --config Release --target test_client_movement --parallel
ctest --test-dir build-msvc -C Release --output-on-failure -R "movement_client|playerdeath|sim"
New-Item -ItemType Directory -Force .agents/tmp/movement-parity/after
.\build-msvc\bin\Release\test_client_movement.exe --headless .install/disc/game.cue .agents/tmp/movement-parity/after
```

The full configured CTest suite passed. A 300-frame headless BASE3 demo produced
identical before/after gameplay reports and framebuffer hashes, excluding the
output path strings. Four-player MATRIX5 headless demos also completed,
including a 600-frame staged combat run exercising deaths and respawns.

## Limits and deliberate behavior

This pass establishes the corrected branches above, not exhaustive parity for
every movement state on every map. The existing non-grounding body-separation
behavior is deliberate: the console's apparent stand-on-entity arm reads an
uninitialized stack word at `0x80051454`. The defined port behavior and the
disassembly evidence are documented in `src/game/trace.h`; this audit does not
replace them with PC Quake II's actor-grounding rules.

Validation also exposed an unrelated Windows startup stack overflow: `main`
allocated the complete client, including its simulation slots, on the default
stack. It now uses static storage for this process-lifetime object, allowing
both normal startup and `--version` to run.
