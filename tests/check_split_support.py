"""Real headless split-screen arena and match-lifecycle regression checks.

Requires a local PAL or USA disc. All output stays in the repository's
.agents/tmp area. Complements check_split_screen.py's HUD/weapon pixel checks.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess

from check_split_screen import ROOT, WEAPON, require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True, type=Path)
    parser.add_argument("--disc", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    client, disc, output = (p.resolve() for p in (args.client, args.disc, args.output))
    require(output.is_relative_to(ROOT / ".agents/tmp"), "output must be under .agents/tmp")
    output.mkdir(parents=True, exist_ok=True)
    records = []

    def run(name, map_name, frames, options):
        shot = output / f"{name}.ppm"
        if shot.exists():
            shot.unlink()
        command = [str(client), "--disc", str(disc), "--headless", "--map", map_name,
                   "--frames", str(frames), "--shot", str(shot), "--saves",
                   str(output / f"saves-{name}"), "--dm", *options]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=120)
        log = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        require(result.returncode == 0, f"{name}: exit {result.returncode}")
        require(shot.is_file(), f"{name}: missing engine capture")
        require("[error]" not in log.lower(), f"{name}: engine error")
        return log

    arenas = [f"MATRIX{i}" for i in range(1, 10)] + ["THEVAT", "TIMS", "PODCITY", "FRAGTOWE"]
    for arena in arenas:
        log = run(arena, arena, 120, ["--dm-players", "4", "--no-crosshair"])
        owners = {int(p): (int(w), int(n), int(live))
                  for p, w, n, _, live in WEAPON.findall(log)}
        require(set(owners) == set(range(4)), f"{arena}: missing owner")
        require(all(n > 0 and live for _, n, live in owners.values()),
                f"{arena}: a stationary spawn died or lost its view weapon")
        positions = re.findall(r"player (\d+) at \[(-?\d+) (-?\d+) (-?\d+)\]", log)
        require(len(positions) == 4, f"{arena}: missing spawn position")
        require(all(abs(int(y)) < 100000 for _, _, y, _ in positions),
                f"{arena}: a player fell out of the arena")
        records.append({"case": arena, "owners": owners, "positions": positions})
        print(f"PASS {arena}: four live players", flush=True)

    for name, mode, split, players in [("deathmatch", 0, "horizontal", 2),
                                       ("team-deathmatch", 1, "vertical", 2),
                                       ("versus", 5, "horizontal", 2),
                                       ("deathmatch-four", 0, "horizontal", 4),
                                       ("team-deathmatch-four", 1, "horizontal", 4)]:
        log = run(name, "MATRIX5", 1800,
                  ["--dm-players", str(players), "--dm-mode", str(mode), "--dm-stage", "--demo",
                   "--dm-frags", "1", "--dm-rounds", "2", "--dm-split", split])
        require("request 11 (load MPResults)" in log, f"{name}: results never opened")
        require("QMRESULT loaded; one results viewport, scores preserved" in log,
                f"{name}: results scene was never loaded")
        require(f"results ready mask {(1 << players) - 1:X}" in log,
                f"{name}: not every player became ready")
        require("all players ready; returning to multiplayer menu" in log,
                f"{name}: results never released")
        rounds = re.findall(r"round restarted (\d+); wins (\d+) (\d+) (\d+) (\d+)", log)
        if mode == 5:
            require(rounds, "Versus ended without playing another round")
            totals = [sum(map(int, row[1:])) for row in rounds]
            require(totals == sorted(totals) and totals[0] == 1,
                    "round wins were lost while reloading the arena")
        else:
            require(not rounds, f"{name}: incorrectly used Versus round restart")
        records.append({"case": name, "rounds": rounds, "returned_to_multiplayer_menu": True})
        print(f"PASS {name}: complete match and all-player continuation", flush=True)

    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    print(f"{len(records)} split-support scenarios passed", flush=True)


if __name__ == "__main__":
    main()
