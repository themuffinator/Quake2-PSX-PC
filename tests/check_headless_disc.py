"""Drive the real client over the whole disc, headlessly, and assert on it.

Every existing check in `tests/` either links a library and calls it directly or
exercises one narrow slice of the client. None of them answers the question a
change to the game actually raises: *does every level on the disc still load,
still render, still run its script, still wake its creatures, and still let a
player shoot one?* This does, by running the shipped client as a subprocess with
`--headless --report` and reading the counters back.

`--headless` is not optional and not a convenience. Without it the client
advances on the wall clock, `dt` varies per frame, and every cumulative number
below is unreproducible; with it the step is a fixed 1/30 s and two runs of the
same case agree exactly.

Both discs are covered and the suite adapts: it asks the client which release
it identified and picks the film names to match, since the three films are
under different names on SLUS-00757.

Run from the repository root:

    python tests/check_headless_disc.py --disc .install/disc/game.cue
    python tests/check_headless_disc.py --disc "<...>/Quake II (USA).cue"

  --client PATH   which binary (default: the staged .install one, else build-msvc)
  --disc PATH     which disc (default: .install/disc/game.cue)
  --output DIR    where logs and captures go (default: .agents/tmp/headless)
  --jobs N        how many runs at once (default: half the CPUs)
  --only PATTERN  run just the cases whose name contains this
  --quick         one zone per map instead of every zone

Exit status is 0 only when every case passed.
"""

import argparse
import concurrent.futures
import json
import os
import re
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
# `\r?$` because the client's stderr reaches here with the host's line endings
# and a bare `$` under re.M would leave the CR inside the captured integer.
REPORT = re.compile(r"^\[\w+\s*\] report\.([a-z_.]+) (-?\d+)\r?$", re.M)

# The playable levels, in disc order. The screens — QFRONT, QLOGOS, QENDMIS*,
# QFMV, QDUMMY, QINTER, QMRESULT, QSTARTUP, QMAG* — are not levels and are
# covered by their own cases below rather than by the map sweep.
CAMPAIGN = [
    "BASE0", "BASE1", "BASE2", "BASE3", "BIGGUN", "BOSS1", "BOSS2", "COMMAND",
    "JAIL2", "JAIL3", "JAIL4", "JAIL5", "LAB", "PODCITY", "POWER1", "POWER2",
    "SECURITY", "THEVAT", "WASTE1", "WASTE2", "WASTE3", "WASTE4",
]

# The arenas. They carry only the multiplayer icon sheets, so they are opened
# with --dm; opening one alone is the disc being right, not a fault.
ARENAS = [
    "FRAGTOWE", "MATRIX1", "MATRIX2", "MATRIX3", "MATRIX4", "MATRIX5",
    "MATRIX6", "MATRIX7", "MATRIX8", "MATRIX9", "TIMS",
]

# How many zones each campaign map has, as the disc has them. Written down
# rather than probed so that a map that LOSES a zone is a failure instead of a
# quietly shorter sweep.
ZONES = {
    "BASE0": 1, "BASE1": 2, "BASE2": 3, "BASE3": 4, "BIGGUN": 4, "BOSS1": 2,
    "BOSS2": 3, "COMMAND": 5, "JAIL2": 3, "JAIL3": 6, "JAIL4": 4, "JAIL5": 4,
    "LAB": 5, "PODCITY": 2, "POWER1": 3, "POWER2": 5, "SECURITY": 3,
    "THEVAT": 2, "WASTE1": 4, "WASTE2": 4, "WASTE3": 3, "WASTE4": 2,
}


class Failure(Exception):
    pass


def parse_report(log):
    return {k: int(v) for k, v in REPORT.findall(log)}


def read_ppm_header(path):
    with open(path, "rb") as f:
        head = f.read(64)
    if not head.startswith(b"P6"):
        raise Failure(f"{path.name} is not a P6 capture")
    fields = head.split()
    return int(fields[1]), int(fields[2])


def frame_is_lit(path, threshold=2.0):
    """Mean sample brightness. A level that rendered nothing is black."""
    with open(path, "rb") as f:
        data = f.read()
    idx, fields = 0, []
    while len(fields) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(data[start:idx])
    idx += 1
    w, h = int(fields[1]), int(fields[2])
    px = data[idx:idx + w * h * 3]
    step = 3 * 37                      # a prime stride, so no row aliasing
    total = count = 0
    for i in range(0, len(px) - 3, step):
        total += px[i] + px[i + 1] + px[i + 2]
        count += 3
    return (total / count) if count else 0.0


class Case:
    def __init__(self, name, args, checks=(), frames=90, shot=True,
                 timeout=180, wants_report=True, needs_gate=None,
                 twice=False):
        self.name = name
        self.args = list(args)
        self.checks = list(checks)
        self.frames = frames
        self.shot = shot
        self.timeout = timeout
        # `--zone-probe` answers a static question and leaves before the frame
        # loop, so it has no run to report on. Its verdict is its own output.
        self.wants_report = wants_report
        # A map name: probe it first and stand the player in a gate's doorway.
        self.needs_gate = needs_gate
        self.expect_zone = None
        self.start_zone = 0
        self.log = ""
        self.twice = twice


GATE_RE = re.compile(
    r"-> '\w+' \(zone (-?\d+)\).*?"
    r"centre \((-?\d+),(-?\d+),(-?\d+)\)"
    r" resolves in zones \[([^\]]*)\]", re.S)


def first_gate(client, disc, map_name):
    """
    Find a zone gate this map can actually be walked through, and say which
    zone to start in to do it.

    A gate is a trigger volume whose event record carries a ZONEGATE, and
    neither the volume nor the record says so from the outside — the probe is
    the only thing that knows. It also prints which zones' hulls hold each
    doorway, which is what makes the choice possible: a gate whose doorway is
    only in zone 1 cannot be stood in from zone 0, and a gate in zone 0 that
    NAMES zone 0 is refused by the runtime before it reaches the loader
    (0x800791E0 compares the name against the resident one).

    Returns (start_zone, "x,y,z", dest_zone), or None for a map with no gates.
    """
    proc = subprocess.run(
        [str(client), "--disc", str(disc), "--headless", "--zone-probe",
         "--map", map_name],
        cwd=ROOT, capture_output=True, timeout=900)
    log = proc.stdout.decode("utf-8", errors="replace")
    best = None
    for dest, x, y, z, holding in GATE_RE.findall(log):
        dest = int(dest)
        zones = [int(v) for v in holding.split()]
        # Stand in a zone that holds the doorway and is not the destination.
        for start in zones:
            if start != dest:
                cand = (start, f"{x},{y},{z}", dest)
                # Prefer starting in zone 0: the fewest moving parts.
                if best is None or (cand[0] == 0 and best[0] != 0):
                    best = cand
                break
    return best


def disc_is_ntsc(client, disc):
    """
    Which disc this is, asked of the client rather than of the filename.

    It matters for more than the banner: the USA build has its own 512x240
    display, its own frame rate and its own names for the three films. The
    client prints the release it identified as its last line.
    """
    proc = subprocess.run(
        [str(client), "--disc", str(disc), "--headless", "--frames", "1",
         "--map", "QDUMMY"],
        cwd=ROOT, capture_output=True, timeout=300)
    log = (proc.stdout + proc.stderr).decode("utf-8", errors="replace")
    return "NTSC" in log or "SLUS" in log


def run_case(case, client, disc, output):
    out = output / case.name
    out.mkdir(parents=True, exist_ok=True)
    shot = out / "frame.ppm"
    if shot.exists():
        shot.unlink()

    if case.needs_gate:
        gate = first_gate(client, disc, case.needs_gate)
        if not gate:
            # Not a failure: PODCITY and THEVAT have two zones and no gate
            # between them — the second is reached some other way.
            return case.name, True, [], {"skipped": 1}
        case.args += ["--zone", str(gate[0]), "--at", gate[1]]
        case.expect_zone = gate[2]
        case.start_zone = gate[0]

    argv = [str(client), "--disc", str(disc), "--headless", "--report",
            "--frames", str(case.frames),
            "--saves", str(out / "saves")] + case.args
    if case.shot:
        argv += ["--shot", str(shot)]

    try:
        proc = subprocess.run(argv, cwd=ROOT, capture_output=True,
                              timeout=case.timeout)
    except subprocess.TimeoutExpired:
        (out / "run.log").write_text("TIMEOUT\n", encoding="utf-8")
        return case.name, False, [f"timed out after {case.timeout}s"], {}

    log = (proc.stdout + proc.stderr).decode("utf-8", errors="replace")
    (out / "run.log").write_text(log, encoding="utf-8")
    report = parse_report(log)
    (out / "report.json").write_text(json.dumps(report, indent=1),
                                     encoding="utf-8")

    problems = []
    if proc.returncode != 0:
        problems.append(f"exit {proc.returncode}")
    if case.wants_report and not report:
        problems.append("no report block")
    if "AddressSanitizer" in log:
        first = next((l for l in log.splitlines() if "AddressSanitizer" in l), "")
        problems.append("sanitizer: " + first.strip()[:120])
    if case.shot:
        if not shot.is_file():
            problems.append("no framebuffer capture")
        else:
            try:
                w, h = read_ppm_header(shot)
                if (w, h) not in ((512, 248), (512, 240)):
                    problems.append(f"capture is {w}x{h}")
            except Failure as exc:
                problems.append(str(exc))

    if case.twice:
        # SAME RUN, TWICE. `--headless` exists so that a scripted run is
        # reproducible — a fixed 1/30 s step instead of the wall clock — and
        # AGENTS.md leans on that for every number this project quotes. Nothing
        # asserted it. Three identical `--frames 500` captures of BASE3 gave
        # 497, 497 and 4567 projectile lights before it existed.
        # The first capture has to be taken aside BEFORE the second run, which
        # writes over it.
        first_bytes = shot.read_bytes() if (case.shot and shot.is_file()) else None
        second = subprocess.run(argv, cwd=ROOT, capture_output=True,
                                timeout=case.timeout)
        log2 = (second.stdout + second.stderr).decode("utf-8", errors="replace")
        (out / "run2.log").write_text(log2, encoding="utf-8")
        report2 = parse_report(log2)
        drift = {k: (v, report2.get(k)) for k, v in report.items()
                 if report2.get(k) != v}
        if drift:
            problems.append("counters differ between two identical runs: "
                            + ", ".join(f"{k} {a} vs {b}"
                                        for k, (a, b) in sorted(drift.items())))
        if first_bytes is not None:
            if not shot.is_file():
                problems.append("the second run wrote no capture")
            elif shot.read_bytes() != first_bytes:
                (out / "frame1.ppm").write_bytes(first_bytes)
                problems.append("the two captures differ")

    case.log = log
    for check in case.checks:
        try:
            check(report if case.wants_report else log, out, shot)
        except Failure as exc:
            problems.append(str(exc))
        except Exception as exc:                      # a check that itself broke
            problems.append(f"check raised {exc!r}")

    return case.name, not problems, problems, report


# --------------------------------------------------------------------------
# The checks. Each takes (report, outdir, shot) and raises Failure.
# --------------------------------------------------------------------------
def no_errors(report, out, shot):
    if report.get("run.errors", 0):
        raise Failure(f"{report['run.errors']} error lines")


def credits_opened(report, out, shot):
    log = (out / "run.log").read_text(encoding="utf-8")
    if not re.search(r"credits, [1-9][0-9]* authored rows", log):
        raise Failure("credits did not load their authored row table")


def credits_finished(report, out, shot):
    credits_opened(report, out, shot)
    log = (out / "run.log").read_text(encoding="utf-8")
    if log.count("credits complete") != 1:
        raise Failure("credits did not finish once and return to Options")


def campaign_finished(report, out, shot):
    credits_opened(report, out, shot)
    log = (out / "run.log").read_text(encoding="utf-8")
    if len(re.findall(r"movie: extro over[^\r\n]*campaign credits", log)) != 1:
        raise Failure("the campaign outro did not enter credits exactly once")
    if len(re.findall(r"credits complete[^\r\n]*unlock file", log)) != 1:
        raise Failure("campaign credits did not reach the unlock question")
    if len(re.findall(r"campaign complete[^\r\n]*SILVER cheats", log)) != 1:
        raise Failure("default Medium difficulty did not award Silver")


def standalone_movie_finished(report, out, shot):
    """--movie must stop on natural completion, before the diagnostic cap."""
    log = (out / "run.log").read_text(encoding="utf-8")
    if not re.search(r"movie: [1-9]\d+ frames shown", log):
        raise Failure("movie never completed")
    if not 100 < report.get("run.frames", 0) < 5000:
        raise Failure("standalone movie ran to the frame cap")
    if "movie: over with no destination" in log or "movie: intro over" in log:
        raise Failure("standalone movie returned to the front end")
    if "campaign complete" in log or "authored rows" in log:
        raise Failure("standalone movie entered the campaign reward flow")


def ticked(report, out, shot):
    if report.get("run.ticks", 0) < 1:
        raise Failure("the world never stepped")


def rendered(report, out, shot):
    if report.get("level.nodes", 0) < 1:
        raise Failure("the zone has no scene nodes")
    if report.get("level.vertices", 0) < 1:
        raise Failure("the zone has no vertices")
    if shot and shot.is_file() and frame_is_lit(shot) < 1.0:
        raise Failure("the frame is black")


def loaded_once(report, out, shot):
    if report.get("level.loads", 0) != 1:
        raise Failure(f"{report.get('level.loads')} zone loads, expected 1")
    if report.get("level.loads_failed", 0):
        raise Failure(f"{report['level.loads_failed']} zone loads failed")


def no_loading_screen(report, out, shot):
    """A run that stands in one zone must never raise the transition screen."""
    if report.get("level.loading_screens", 0):
        raise Failure(f"{report['level.loading_screens']} loading screens in a"
                      " run that never changed zone")


def screens_only_for_level_changes(report, out, shot):
    """
    ONE SCREEN PER LEVEL CHANGE, AND NOTHING FOR ANYTHING ELSE.

    The level screen is the vblank hook TestIt installs at 0x8006E144 around
    LoadLevel, and it is the only one this port raises. A zone gate is
    seamless; so is a load that changed no directory at all -- a death, a
    restart, an arena round reset, a memory-card restore.

    The first load of a run is setup and raises nothing, so the ceiling is
    loads - 1.
    """
    screens = report.get("level.loading_screens", 0)
    loads = report.get("level.loads", 0)
    if screens > max(loads - 1, 0):
        raise Failure(f"{screens} loading screens for {loads} loads")


def crossed_one_gate(case):
    """
    A real zone gate, walked through, shows NOTHING.

    MaybeLoadZoneName (0x80079178) raises the console's still to cover a CD
    read this port does not do; a zone seam here is meant to pass without the
    player seeing anything. The gate still has to happen and still has to load,
    which is what the other two checks are for.
    """
    def check(report, out, shot):
        gates = len(re.findall(r"zone gate -> zone \d+", case.log))
        if gates < 1:
            raise Failure("the player never crossed a zone gate")
        screens = report.get("level.loading_screens", 0)
        if screens != 0:
            raise Failure(f"{screens} loading screens for {gates} zone gates,"
                          " which must be seamless")
        if report.get("level.loads", 0) < 1 + gates:
            raise Failure("a gate fired without a load behind it")
    return check


def bodies_blocked_something(report, out, shot):
    """
    Creatures are solid. `bound_trace`'s actor arm (0x8005BF4C) is what stops a
    creature's step against the player or another creature, and before it
    existed this counter could only ever be zero.
    """
    if report.get("creatures.traces", 0) < 1:
        raise Failure("no creature ever traced a step")
    if report.get("creatures.blocked_body", 0) < 1:
        raise Failure("not one creature step was stopped by another body")
    # ...and the clip must not be so eager that nothing can walk any more. A
    # creature already overlapping a body has every step refused, which is the
    # console's behaviour too; a creature that never moves at all is not.
    if report.get("creatures.moved", 0) < 1:
        raise Failure("no creature moved from where it spawned")


def clock_runs_once_a_frame(report, out, shot):
    """
    ONE WORLD, ONE CLOCK, whatever the player count.

    `q2_sim_tick` runs once per player against one world and the world half is
    gated on being player 0. `sim->level_time += dt` was not, so a two-player
    match aged the world twice as fast and a four-player one four times — every
    powerup's duration, every scripted mover wait, the pickup caption, Mega
    Health's decay, the hazard throttle and the drowning deadline all read that
    clock.

    Asserted as a RATE rather than against a number, because the number depends
    on how the accumulator lands: a tick consumes at least the build's frame
    (12 units on PAL, 10 on NTSC) and at most a capped step. Twenty-five is
    comfortably above the honest ceiling and far below the 77 per tick a
    four-player match was running at.
    """
    ticks = report.get("run.ticks", 0)
    clock = report.get("run.level_time", 0)
    if ticks < 1:
        raise Failure("the world never stepped")
    if clock > ticks * 25:
        raise Failure(f"the level clock ran at {clock / ticks:.1f} units a tick,"
                      " which is more than one player's worth")


def script_did_something(report, out, shot):
    """
    The counters belong to the level the run ENDED in, not to the one it
    started in — `q2_sim_attach_gameplay` builds a fresh event runtime at every
    load — so a map whose triggers include a LOADMAP legitimately finishes with
    `script.calls` at zero. What must be true either way is that firing every
    trigger on the map did *something*: ran a call, woke a batch, or took the
    session somewhere else.
    """
    did = (report.get("script.calls", 0) or report.get("script.summoned", 0)
           or report.get("script.units", 0) or report.get("script.teleports", 0)
           or report.get("level.loads", 0) > 1)
    if not did:
        raise Failure("firing every trigger on the map changed nothing")
    if report.get("level.loads_failed", 0):
        raise Failure(f"{report['level.loads_failed']} loads failed")


def probe_reached_every_zone(count):
    """The probe walks zones until one is absent; it must reach them all."""
    def check(log, out, shot):
        seen = set(int(z) for z in re.findall(r"^  --- zone (\d+):", log, re.M))
        missing = set(range(count)) - seen
        if missing:
            raise Failure("the probe never reached zone(s) "
                          + ", ".join(str(z) for z in sorted(missing)))
    return check


def probe_gates_resolve(log, out, shot):
    """Every zone gate must name a zone the map has and land in a cell."""
    bad = []
    for name, dest in re.findall(r"-> '(\w+)' \(zone (-?\d+)\)", log):
        if int(dest) < 0:
            bad.append(f"{name} names no zone")
    nowhere = re.findall(r"cell in DEST (-?\d+) = -1", log)
    if bad:
        raise Failure("; ".join(sorted(set(bad))))
    if nowhere:
        # Not a failure on its own: a doorway may legitimately sit outside the
        # destination hull. Recorded so a regression in placement is visible.
        (out / "gates-outside-dest.txt").write_text(
            f"{len(nowhere)} gate doorways resolve to no cell in their"
            " destination\n", encoding="utf-8")


def expect(key, at_least=None, exactly=None):
    def check(report, out, shot):
        value = report.get(key)
        if value is None:
            raise Failure(f"no {key} in the report")
        if exactly is not None and value != exactly:
            raise Failure(f"{key} is {value}, expected {exactly}")
        if at_least is not None and value < at_least:
            raise Failure(f"{key} is {value}, expected at least {at_least}")
    return check


def build_cases(quick, ntsc):
    cases = []
    base = [no_errors, ticked, rendered, loaded_once, no_loading_screen]

    for name in CAMPAIGN:
        zones = 1 if quick else ZONES[name]
        for z in range(zones):
            cases.append(Case(f"map-{name}-z{z}",
                              ["--map", name, "--zone", str(z)],
                              checks=base))

    for name in ARENAS:
        cases.append(Case(f"arena-{name}",
                          ["--map", name, "--dm", "--dm-players", "2"],
                          checks=[no_errors, ticked, rendered]))

    # The zone gates of every multi-zone map, as a static question.
    for name in CAMPAIGN:
        if ZONES[name] > 1:
            cases.append(Case(f"gates-{name}", ["--map", name, "--zone-probe"],
                              checks=[probe_reached_every_zone(ZONES[name]),
                                      probe_gates_resolve],
                              frames=1, shot=False, wants_report=False,
                              timeout=600))

    # Creatures: a level with a firefight in it, driven by the demo pad.
    cases.append(Case("fight-SECURITY",
                      ["--map", "SECURITY", "--demo", "--shoot", "--god"],
                      frames=600,
                      checks=[no_errors, ticked, rendered,
                              expect("creatures.live", at_least=1),
                              expect("creatures.thoughts", at_least=1),
                              expect("player.shots", at_least=1),
                              bodies_blocked_something],
                      timeout=300))
    cases.append(Case("fight-BASE1",
                      ["--map", "BASE1", "--demo", "--shoot", "--god",
                       "--weapon", "3"],
                      frames=600,
                      checks=[no_errors, ticked, rendered,
                              expect("creatures.thoughts", at_least=1)],
                      timeout=300))

    # The script, on every map: fire every trigger volume once and let the
    # consequences run. This is the only case that crosses ZONE GATES — it is
    # what walks the load path a second and third time in one process, which is
    # where a pointer kept across a COMMON.DAT swap shows up.
    for name in CAMPAIGN:
        cases.append(Case(f"script-{name}",
                          ["--map", name, "--fire-triggers"],
                          frames=300,
                          checks=[no_errors, script_did_something,
                                  screens_only_for_level_changes],
                          timeout=600))

    # WALKING THROUGH A ZONE GATE, on every map that has one. The probe says
    # where a doorway is and the case stands the player in it; what is asserted
    # is that the crossing happened and that it raised exactly one screen.
    for name in CAMPAIGN:
        if ZONES[name] > 1:
            case = Case(f"cross-{name}", ["--map", name],
                        frames=90, needs_gate=name, timeout=900)
            case.checks = [no_errors, crossed_one_gate(case)]
            cases.append(case)

    # Split screen, both layouts and every player count.
    for players in (2, 3, 4):
        cases.append(Case(f"split-{players}p",
                          ["--map", "MATRIX5", "--dm",
                           "--dm-players", str(players)],
                          frames=300,
                          checks=[no_errors, ticked, rendered,
                                  clock_runs_once_a_frame]))
    for layout in ("horizontal", "vertical"):
        cases.append(Case(f"split-2p-{layout}",
                          ["--map", "MATRIX5", "--dm", "--dm-players", "2",
                           "--dm-split", layout],
                          frames=300,
                          checks=[no_errors, ticked, rendered,
                                  clock_runs_once_a_frame]))

    # THE HUD, one case per thing that changes it. None of these asserts what
    # the overlay LOOKS like — that is what the inspectors are for — but a run
    # that errors or draws nothing is a fault whatever the pixels were meant to
    # be, and before this the carousel, the powerup shells, the key row and the
    # objectives pop-up had no headless coverage at all.
    for kind in ("quad", "invuln", "enviro", "breather"):
        cases.append(Case(f"powerup-{kind}",
                          ["--map", "BASE1", "--powerup", kind, "--god"],
                          frames=90,
                          checks=[no_errors, ticked, rendered]))
    for weapon in range(1, 12):
        cases.append(Case(f"weapon-{weapon:02d}",
                          ["--map", "BASE1", "--weapon", str(weapon),
                           "--shoot", "--god"],
                          frames=90,
                          checks=[no_errors, ticked, rendered]))
    cases.append(Case("hud-keys", ["--map", "JAIL2", "--keys", "--armour",
                                   "body"],
                      frames=90, checks=[no_errors, ticked, rendered]))
    cases.append(Case("hud-objectives", ["--map", "BASE1", "--objectives", "20"],
                      frames=120, checks=[no_errors, ticked, rendered]))
    cases.append(Case("credits", ["--credits"], frames=300,
                      checks=[no_errors, credits_opened], timeout=300))
    cases.append(Case("credits-complete", ["--credits"], frames=2000,
                      checks=[no_errors, credits_finished], timeout=300))
    cases.append(Case("campaign-unlock", ["--map", "Extro FMV"], frames=5000,
                      checks=[no_errors, campaign_finished], timeout=300))

    # A creature framed, shot at and watched as it dies, with the trace on. The
    # `--trace-cre` path is a different code path through the AI and has never
    # been exercised by anything automated.
    cases.append(Case("watch-SECURITY",
                      ["--map", "SECURITY", "--watch", "--watch-hold", "60",
                       "--shoot", "--god", "--weapon", "3", "--trace-cre", "0"],
                      frames=600,
                      checks=[no_errors, ticked, rendered,
                              expect("creatures.thoughts", at_least=1)],
                      timeout=600))

    # THE SAME RUN, TWICE. See the note in run_case: this is the only case that
    # tests what --headless is for.
    for name, extra in (("BASE1", ["--demo", "--shoot"]),
                        ("SECURITY", ["--demo", "--shoot", "--god"]),
                        ("BASE3", ["--demo"])):
        cases.append(Case(f"repeat-{name}", ["--map", name] + extra,
                          frames=300, twice=True,
                          checks=[no_errors, ticked, rendered],
                          timeout=600))

    # A quick save taken mid-level and restored. It is a LOAD, and it must not
    # raise the transition screen: a memory-card restore is the outer state
    # machine's, not 0x80079178's.
    # Every campaign map exercises its own module bindings and creature roster.
    # A BASE1-only check cannot expose an unresolved animation in another module.
    for name in CAMPAIGN:
        case_name = "save-load" if name == "BASE1" else f"save-load-{name}"
        cases.append(Case(case_name, ["--map", name, "--save-load", "40"],
                          frames=120,
                          checks=[no_errors, ticked, rendered,
                                  no_loading_screen,
                                  expect("level.loads", exactly=2),
                                  expect("level.loads_failed", exactly=0)],
                          timeout=300))

    # The screens, which are not levels: they have no world and must not be
    # asked for one, but they must still compose a frame and exit clean.
    cases.append(Case("front-end", ["--map", "QFRONT"], frames=120,
                      checks=[no_errors]))
    cases.append(Case("boot-chain", ["--boot"], frames=300,
                      checks=[no_errors], timeout=300))
    cases.append(Case("new-game", ["--new-game"], frames=300,
                      checks=[no_errors], timeout=300))
    for unit in range(1, 6):
        cases.append(Case(f"endmis-{unit}", ["--map", f"QENDMIS{unit}"],
                          frames=90, checks=[no_errors]))

    # The films, whose names are different on the two discs: the NTSC modules
    # cut them in different places and the disc carries them under their own
    # names. `--movie` with a name this disc does not have exits 1, which is
    # what makes running the wrong set a real failure rather than a warning.
    films = (("TAKE1BP.STX", "OUTRO1P.STX", "ROGUEINP.STX") if ntsc is False
             else ("TAKE1B.STX", "OUTRO1.STX", "ROGUEIN1.STX"))
    for film in films:
        cases.append(Case(f"movie-{film.split('.')[0]}", ["--movie", film],
                          frames=200, checks=[no_errors], timeout=300))
    cases.append(Case("movie-natural-exit", ["--movie", films[0]],
                      frames=5000, checks=[no_errors, standalone_movie_finished],
                      timeout=300))
    cases.append(Case("movie-outro-natural-exit", ["--movie", films[1]],
                      frames=5000, checks=[no_errors, standalone_movie_finished],
                      timeout=300))

    return cases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", default=str(ROOT / ".install/disc/game.cue"))
    ap.add_argument("--client", default=None)
    ap.add_argument("--output", default=str(ROOT / ".agents/tmp/headless"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--only", default=None)
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()

    client = args.client
    if not client:
        for candidate in (ROOT / ".install/q2psx.exe",
                          ROOT / "build-msvc/bin/Release/q2psx.exe",
                          ROOT / "build-client/bin/q2psx",
                          ROOT / "build/bin/q2psx"):
            if candidate.is_file():
                client = str(candidate)
                break
    if not client or not Path(client).is_file():
        print("no client binary; pass --client", file=sys.stderr)
        return 2
    # Absolute, because the runs are spawned with cwd=ROOT and a relative
    # executable would be resolved against this process's directory instead.
    client = str(Path(client).resolve())
    disc = Path(args.disc)
    if not disc.is_file():
        print(f"no disc at {disc}; pass --disc", file=sys.stderr)
        return 2
    disc = disc.resolve()

    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)

    ntsc = disc_is_ntsc(client, disc)
    print(f"disc is {'NTSC (SLUS-00757)' if ntsc else 'PAL (SLES-01534)'}")
    cases = build_cases(args.quick, ntsc)
    if args.only:
        cases = [c for c in cases if args.only in c.name]
    if not cases:
        print("no cases match", file=sys.stderr)
        return 2

    print(f"{len(cases)} cases, {args.jobs} at a time, client {client}")
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_case, c, client, disc, output): c
                   for c in cases}
        for future in concurrent.futures.as_completed(futures):
            name, ok, problems, report = future.result()
            results.append((name, ok, problems, report))
            print(f"  {'ok  ' if ok else 'FAIL'}  {name}"
                  + ("" if ok else "  -- " + "; ".join(problems)))

    results.sort()
    (output / "summary.json").write_text(json.dumps(
        [{"case": n, "ok": ok, "problems": p, "report": r}
         for n, ok, p, r in results], indent=1), encoding="utf-8")

    failed = [n for n, ok, _, _ in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    if failed:
        print("failed: " + ", ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
