#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Measure simulation-only quiet and combat runs, with repeat digest checks.

Writes JSON to stdout. Each process gets fresh preferences and a working folder
under --scratch-root; logs remain there for inspection. Engine loop timing
includes logging. Process wall timing also includes startup, loading and exit.
No game assets are copied. --self-test checks reporting without game assets.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time

RUN_LINE = re.compile(
    r"simulation run: (\d+) ticks in ([\d.]+) s "
    r"\(([\d.]+) ticks/s; logging included\) world digest ([0-9a-fA-F]{16})"
)


def measure(command, workdir, ticks, timeout):
    """Run a bounded child and retain logs, elapsed time and any reported result."""
    environment = os.environ.copy()
    environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    started = time.perf_counter()
    record = {"command": command, "log": str(workdir / "process.log")}
    with (workdir / "process.log").open("w", encoding="utf-8") as output:
        try:
            child = subprocess.run(command, cwd=workdir, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT,
                                   timeout=timeout, check=False)
            record["returncode"] = child.returncode
        except subprocess.TimeoutExpired:
            record["error"] = f"process exceeded {timeout} seconds"
        except OSError as error:
            record["error"] = str(error)
    record["process_wall_seconds"] = time.perf_counter() - started
    lines = RUN_LINE.findall((workdir / "process.log").read_text(
        encoding="utf-8", errors="replace"))
    if "error" in record:
        return record
    if record["returncode"] != 0:
        record["error"] = f"process exited with {record['returncode']}"
    elif len(lines) != 1:
        record["error"] = f"expected one simulation result, found {len(lines)}"
    else:
        actual_ticks, seconds, throughput, digest = lines[0]
        actual_ticks, seconds, throughput = int(actual_ticks), float(seconds), float(throughput)
        record.update(ticks=actual_ticks, engine_loop_seconds=seconds,
                      engine_loop_ticks_per_second=throughput, world_digest=digest.lower())
        if actual_ticks != ticks:
            record["error"] = f"expected {ticks} ticks, got {actual_ticks}"
        elif seconds <= 0 or throughput <= 0:
            record["error"] = "engine timing must be positive; use more ticks"
        else:
            record["process_wall_ticks_per_second"] = actual_ticks / record["process_wall_seconds"]
            record["process_outside_loop_seconds"] = record["process_wall_seconds"] - seconds
    return record


def benchmark(args):
    """Run the same seeded workloads independently and compare their digests."""
    native, game_dir = args.native.resolve(), args.game_dir.resolve()
    if not native.is_file() or not game_dir.is_dir():
        raise ValueError("--native must name a file and --game-dir must name a directory")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="simulation-benchmark-", dir=args.scratch_root.resolve()))
    binary_hash = hashlib.sha256()
    with native.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            binary_hash.update(block)
    digest = binary_hash.hexdigest()
    report = {
        "schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
        "engine": {"path": str(native), "sha256": digest},
        "host": {"platform": platform.platform(), "machine": platform.machine(),
                 "logical_cpus": os.cpu_count()},
        "config": {"game_dir": str(game_dir), "ticks": args.ticks, "seed": args.seed,
                   "repeats": args.repeats, "combat_units": args.combat_units,
                   "timeout_seconds": args.timeout, "scratch": str(workdir)},
        "timing_note": "Engine loop includes logging; process wall includes startup, loading and exit.",
        "cases": [],
    }
    for name, extra in (("quiet", []), ("combat", ["--combat", str(args.combat_units),
                                                   "--busy-combat"])):
        runs = []
        for repeat in range(args.repeats):
            folder = workdir / f"{name}-{repeat + 1}"
            folder.mkdir()
            preferences = folder / "preferences.conf"
            preferences.write_text("open-annihilation-preferences 1\n", encoding="utf-8")
            command = [str(native), "--game-dir", str(game_dir), "--headless-check",
                       "--simulation-only", "--skip-intro", "--mute",
                       "--preferences-file", str(preferences), "--match-ticks", str(args.ticks),
                       "--seed", str(args.seed), *extra]
            runs.append(measure(command, folder, args.ticks, args.timeout))
        valid = all("error" not in run for run in runs)
        repeat_equal = valid and len({run["world_digest"] for run in runs}) == 1
        report["cases"].append({"name": name, "runs": runs,
                                 "repeat_digests_equal": repeat_equal})
    report["ok"] = all(case["repeat_digests_equal"] for case in report["cases"])
    return report


def self_test():
    """Check subprocess failure, timeout, timing parsing and wrong tick rejection."""
    with tempfile.TemporaryDirectory(prefix="simulation-benchmark-selftest-") as temporary:
        folder = Path(temporary)
        line = "simulation run: 30 ticks in 0.050000 s (600.00 ticks/s; logging included) world digest 0123456789abcdef"
        command = [sys.executable, "-c", f"print({line!r})"]
        good = measure(command, folder, 30, 10)
        checks = [good.get("world_digest") == "0123456789abcdef",
                  good.get("engine_loop_ticks_per_second") == 600.0,
                  good.get("process_wall_ticks_per_second", 0) > 0,
                  "error" in measure(command, folder, 31, 10),
                  "error" in measure([sys.executable, "-c", "print('no result')"], folder, 30, 10),
                  "error" in measure([sys.executable, "-c", "raise SystemExit(2)"], folder, 30, 10),
                  "error" in measure([sys.executable, "-c", "import time; time.sleep(60)"], folder, 30, 0.05)]
    if not all(checks):
        raise ValueError(f"self-test checks failed: {checks}")
    print("simulation benchmark self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--ticks", type=int, default=300)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--combat-units", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not all((args.native, args.game_dir, args.scratch_root)):
        parser.error("--native, --game-dir and --scratch-root are required")
    if (args.ticks <= 0 or args.repeats < 2 or args.combat_units <= 0 or
            args.timeout <= 0 or not math.isfinite(args.timeout) or not 0 <= args.seed <= 0xffffffff):
        parser.error("ticks/combat-units/timeout must be positive, repeats >= 2 and seed a uint32")
    report = benchmark(args)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
