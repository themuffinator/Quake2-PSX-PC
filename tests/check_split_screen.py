"""Exercise the real client's split HUD and view weapons using a local disc.

Run from the repository, for example:
  python tests/check_split_screen.py --client build/bin/q2psx \
      --disc /path/to/game.cue --output .agents/tmp/split-screen/PAL

All captures use the engine framebuffer and a fixed headless timestep. No
window, input device, installed profile or external screenshot tool is used.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
WEAPON = re.compile(
    r"player (\d+) view weapon (\d+), (\d+) prims, (\d+) fire clips, linked (\d+)"
)
SHOT = re.compile(r"player (\d+) shots (\d+), dry (\d+)")
FLASH = re.compile(r"player (\d+) damage flashes (\d+)")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(client, disc, output, name, players=0, split=None, weapon=2,
             frames=120, fight=False, crosshair=False):
    shot = output / f"{name}.ppm"
    capture = (output / f"{name}_{((frames - 1) // 30) * 30:04d}.ppm"
               if fight else shot)
    if capture.exists():
        capture.unlink()
    args = [str(client), "--disc", str(disc), "--headless", "--map",
            "MATRIX5" if players else "BASE1", "--frames", str(frames),
            "--shot", str(shot), "--saves", str(output / f"saves-{name}")]
    if players:
        args += ["--dm", "--dm-players", str(players)]
    if split:
        args += ["--dm-split", split]
    if players and not fight:
        args += ["--crosshair" if crosshair else "--no-crosshair"]
    if fight:
        args += ["--dm-stage", "--demo", "--shot-every", "30"]
    else:
        args += ["--weapon", str(weapon), "--armour", "body"]
    result = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=120)
    log = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    (output / f"{name}.log").write_text(log, encoding="utf-8")
    require(result.returncode == 0, f"{name}: client exited {result.returncode}")
    require(capture.is_file(), f"{name}: no engine framebuffer capture")
    data = capture.read_bytes()
    require(data.startswith(b"P6"), f"{name}: capture is not P6")
    owners = {int(p): tuple(map(int, (w, n, f, live)))
              for p, w, n, f, live in WEAPON.findall(log)}
    shots = {int(p): int(n) for p, n, _ in SHOT.findall(log)}
    flashes = {int(p): int(n) for p, n in FLASH.findall(log)}
    if players:
        require(set(owners) == set(range(players)), f"{name}: missing viewport owner")
        for p, (w, n, _, live) in owners.items():
            if not fight:
                require(w == (weapon if p == 0 else 1),
                        f"{name}: player {p} borrowed another player's weapon ({w})")
                require(n > 0 and live, f"{name}: player {p} has no rendered weapon")
            elif not live:
                require(n == 0, f"{name}: dead player {p} retained their weapon")
        if fight:
            require(all(shots.get(p, 0) > 0 for p in range(players)),
                    f"{name}: at least one player's animation never fired")
            require(flashes.get(1, 0) > 0, f"{name}: injured player 1 never flashed")
        else:
            require(all(flashes.get(p) == 0 for p in range(players)),
                    f"{name}: an untouched player flashed")
    record = {"case": name, "sha256": hashlib.sha256(data).hexdigest(),
              "capture": capture.name, "weapons": owners, "shots": shots,
              "damage_flashes": flashes}
    print(f"PASS {name}", flush=True)
    return record, log


def check_crosshairs(base, with_crosshair, players, split):
    def pixels(path):
        data = path.read_bytes()
        header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\n", data)
        require(header is not None, f"{path}: unsupported P6 header")
        return int(header[1]), int(header[2]), data[header.end():]

    width, height, before = pixels(base)
    w, h, after = pixels(with_crosshair)
    require((w, h, len(after)) == (width, height, len(before)),
            "crosshair capture changed framebuffer dimensions")
    half_h = (height + 1) // 2 - 1
    if split == "horizontal":
        views = [(0, 1, 512, half_h), (0, 121, 512, half_h)]
    elif split == "vertical":
        views = [(0, 0, 255, height), (256, 0, 255, height)]
    else:
        views = [(1, 1, 256, half_h), (257, 1, 256, half_h),
                 (1, height // 2, 256, half_h), (257, height // 2, 256, half_h)]
    boxes = [(x + w // 2 - 8, y + h // 2 - 8) for x, y, w, h in views[:players]]
    changed = [0] * players
    for offset in range(0, len(before), 3):
        if before[offset:offset + 3] == after[offset:offset + 3]:
            continue
        x, y = (offset // 3) % width, (offset // 3) // width
        owner = next((p for p, (bx, by) in enumerate(boxes)
                      if bx <= x < bx + 16 and by <= y < by + 16), None)
        require(owner is not None, f"crosshair changed a pixel outside its view centre: {x},{y}")
        changed[owner] += 1
    require(all(changed), "at least one viewport has no visible crosshair pixels")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--disc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline", type=Path,
                        help="optional previous executable for the single-player comparison")
    parser.add_argument("--weapon-sweep", action="store_true",
                        help="also capture all eleven weapons in the four-player layout")
    args = parser.parse_args()
    client, disc, output = (p.resolve() for p in (args.client, args.disc, args.output))
    require(output.is_relative_to(ROOT / ".agents" / "tmp"),
            "put captures under this repository's .agents/tmp directory")
    output.mkdir(parents=True, exist_ok=True)
    records = []
    single, _ = run_case(client, disc, output, "single")
    records.append(single)
    if args.baseline:
        baseline, _ = run_case(args.baseline.resolve(), disc, output, "baseline-single")
        require(single["sha256"] == baseline["sha256"], "single-player pixels regressed")
        records.append(baseline)

    layouts = [("two-h", 2, "horizontal"), ("two-v", 2, "vertical"),
               ("three", 3, None), ("four", 4, None)]
    for name, players, split in layouts:
        record, _ = run_case(client, disc, output, name, players, split)
        records.append(record)
    repeated, _ = run_case(client, disc, output, "four-repeat", 4)
    require(repeated["sha256"] == records[-1]["sha256"],
            "repeated headless four-player capture is not deterministic")
    records.append(repeated)

    for name, players, split in layouts:
        record, _ = run_case(client, disc, output, name + "-crosshair", players,
                             split, crosshair=True)
        record["crosshair_pixels"] = check_crosshairs(
            output / (name + ".ppm"), output / record["capture"], players, split)
        records.append(record)

    fight, log = run_case(client, disc, output, "fight", 2, "horizontal",
                         frames=301, fight=True)
    dead_views = [(int(p), int(n)) for p, _, n, _, live in WEAPON.findall(log)
                  if live == "0"]
    require("player 1 died:" in log and (1, 0) in dead_views,
            "fight did not exercise removal of the second player's weapon")
    require(all(n == 0 for _, n in dead_views), "a dead viewport retained a weapon")
    require(fight["weapons"][0][3] == 1 and fight["weapons"][0][1] > 0,
            "second player's death removed the survivor's weapon")
    records.append(fight)

    if args.weapon_sweep:
        for weapon in range(1, 12):
            record, _ = run_case(client, disc, output, f"weapon-{weapon:02}",
                                 4, weapon=weapon)
            records.append(record)

    invalid = subprocess.run([str(client), "--dm-split", "diagonal"],
                             cwd=ROOT, capture_output=True, timeout=10)
    require(invalid.returncode == 2, "invalid split must be rejected before loading")
    (output / "results.json").write_text(json.dumps(records, indent=2) + "\n",
                                          encoding="utf-8")
    print(f"{len(records)} scenarios passed; invalid split rejected", flush=True)


if __name__ == "__main__":
    main()
