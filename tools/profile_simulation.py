#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Run persistent movement, natural AI and combat simulation workloads.

Movement uses absolute coordinates on the fresh-preference Canal Crossing map,
with repeated normal group moves. It is a pathfinding stress case, not a normal
match or proof of a pathfinding hotspot. AI leaves the local player idle while
the built-in opponent develops. Report activity windows to reveal late idle
periods. Optional gprofng collection is separate from unprofiled throughput.
Calibrate its sampling interval on your host before trusting CPU totals: this
environment delivered only 10 samples/CPU-second even when 1 ms was requested.
The default 100 ms avoids understated sample weights here; collect at least
100 CPU-seconds for roughly 1000 samples and inspect collector warnings.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from benchmark_simulation import measure

WINDOW = re.compile(
    r"tick\s+(\d+): ([\d.]+) ms avg ([\d.]+) ms max \| units (\d+) "
    r"projectiles (\d+) wrecks (\d+) explosions (\d+) fragments (\d+) particles (\d+)"
)


def movement_stage(ticks, units):
    """Place four squads and repeatedly send each between separated points."""
    lines = ["# Canal Crossing movement stress; fresh preferences required."]
    for squad in range(4):
        lines.append(f"group squad{squad}")
        for index in range(units):
            x = 300 + squad * 160 + (index % 4) * 24
            z = 1450 + (index // 4) * 24
            lines.append(f"place 0 ARMPW {x} {z}")
    for tick in range(1, ticks + 1, 300):
        z = 1200 if ((tick - 1) // 300) % 2 == 0 else 1850
        for squad in range(4):
            lines.append(f"at {tick} move squad{squad} {350 + squad * 160} {z}")
    return "\n".join(lines) + "\n"


def run(args):
    native, game = args.native.resolve(), args.game_dir.resolve()
    if not native.is_file() or not game.is_dir():
        raise ValueError("native file or game directory missing")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="simulation-profile-", dir=args.scratch_root.resolve()))
    binary_hash = hashlib.sha256()
    with native.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            binary_hash.update(block)
    report = {"schema_version": 1, "native": str(native), "native_sha256": binary_hash.hexdigest(),
              "game_dir": str(game), "seed": args.seed, "scratch": str(root),
              "profiled": args.gprofng, "gprofng_interval_ms": args.interval_ms if args.gprofng else None,
              "note": "Profiled timings include collector overhead; inspect activity windows before comparing cases.",
              "runs": []}
    cases = ["movement", "ai", "combat"] if args.case == "all" else [args.case]
    for case in cases:
        ticks = args.ai_ticks if case == "ai" else args.ticks
        for repeat in range(args.repeats):
            folder = root / f"{case}-{repeat + 1}"
            folder.mkdir()
            preferences = folder / "preferences.conf"
            preferences.write_text("open-annihilation-preferences 1\n", encoding="utf-8")
            command = [str(native), "--game-dir", str(game), "--headless-check", "--simulation-only",
                       "--skip-intro", "--mute", "--preferences-file", str(preferences),
                       "--seed", str(args.seed), "--match-ticks", str(ticks)]
            stage_hash = None
            if case == "movement":
                stage = folder / "movement.stage"
                stage.write_text(movement_stage(ticks, args.squad_units), encoding="utf-8")
                stage_hash = hashlib.sha256(stage.read_bytes()).hexdigest()
                command += ["--stage", str(stage)]
            elif case == "combat":
                command += ["--combat", str(args.combat_units), "--busy-combat"]
            if args.gprofng:
                collector = shutil.which("gprofng")
                if collector is None:
                    raise ValueError("gprofng is unavailable")
                command = [collector, "collect", "app", "-p", str(args.interval_ms),
                           "-o", str(folder / "profile.er"), *command]
            before_cpu = os.times()
            result = measure(command, folder, ticks, args.timeout)
            after_cpu = os.times()
            result["child_process_cpu_seconds"] = (after_cpu.children_user + after_cpu.children_system -
                                                    before_cpu.children_user - before_cpu.children_system)
            result.update(case=case, repeat=repeat + 1, stage_sha256=stage_hash)
            text = Path(result["log"]).read_text(encoding="utf-8", errors="replace")
            names = ("tick", "average_ms", "maximum_ms", "units", "projectiles", "wrecks",
                     "explosions", "fragments", "particles")
            result["activity_windows"] = [dict(zip(names, [int(values[0]), float(values[1]),
                float(values[2]), *(int(value) for value in values[3:])])) for values in WINDOW.findall(text)]
            ordered = [int(value) for value in re.findall(
                r"stage: move squad\d+: (\d+) units to", text)]
            if ordered:
                result["movement_orders"] = {"groups_ordered": len(ordered),
                                              "minimum_live_units_ordered": min(ordered),
                                              "maximum_live_units_ordered": max(ordered)}
            if args.gprofng:
                experiment = folder / "profile.er"
                metadata = experiment / "log.xml"
                if metadata.is_file():
                    collector_log = metadata.read_text(errors="replace").replace("\0", "")
                    result["collector_warnings"] = re.findall(
                        r'<event kind="cwarn"[^>]*>(.*?)</event>', collector_log)
                functions = folder / "functions.txt"
                with functions.open("w", encoding="utf-8") as output:
                    summary = subprocess.run([collector, "display", "text", "-functions",
                                              str(experiment)], stdout=output,
                                             stderr=subprocess.STDOUT, timeout=30, check=False)
                result["profile_functions"] = str(functions)
                result["profile_summary_returncode"] = summary.returncode
            report["runs"].append(result)
    report["repeat_digests_equal"] = {}
    for case in cases:
        results = [item for item in report["runs"] if item["case"] == case]
        report["repeat_digests_equal"][case] = (all("error" not in item for item in results) and
            len({item["world_digest"] for item in results}) == 1) if args.repeats > 1 else None
    report["ok"] = all("error" not in item for item in report["runs"])
    if args.repeats > 1:
        report["ok"] = report["ok"] and all(report["repeat_digests_equal"].values())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--case", choices=("movement", "ai", "combat", "all"), default="all")
    parser.add_argument("--ticks", type=int, default=6000)
    parser.add_argument("--ai-ticks", type=int, default=18000)
    parser.add_argument("--squad-units", type=int, default=16)
    parser.add_argument("--combat-units", type=int, default=200)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--gprofng", action="store_true")
    parser.add_argument("--interval-ms", type=float, default=100)
    args = parser.parse_args()
    if (args.ticks <= 0 or args.ai_ticks <= 0 or not 1 <= args.squad_units <= 60 or
            args.combat_units <= 0 or args.repeats <= 0 or args.timeout <= 0 or args.interval_ms <= 0 or
            not math.isfinite(args.timeout) or not math.isfinite(args.interval_ms) or
            not 0 <= args.seed <= 0xffffffff):
        parser.error("counts, timeout and interval must be positive; squad-units must be 1..60")
    result = run(args)
    print(json.dumps(result, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
