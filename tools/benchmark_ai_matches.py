#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Measure native opposing AI matches and replay the resulting .tad recordings.

Use --recording FILE.tad to benchmark an existing recording instead. JSON goes
on stdout; commands, recordings and process logs remain under --scratch-root.
Live match timing includes two simulations and loopback networking. Replay
runs existing recorded decisions and does not measure the cost of running AI.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

NORMAL_TICKS_PER_SECOND = 30
AI_RESULT = re.compile(
    r"AI benchmark: (\d+) / (\d+) ticks in ([\d.]+) s "
    r"\(([\d.]+) ticks/s; ([\d.]+)x normal\), peak units (\d+), "
    r"defeated ([01]), record errors (\d+) / (\d+)"
)
AI_WINDOW = re.compile(
    r"AI window: tick (\d+), units (\d+) / (\d+), kills (-?\d+) / (-?\d+), ([\d.]+) ticks/s"
)
AI_STATE = re.compile(
    r"AI state: tick (\d+), rules (\d+), participating ([01]) / ([01]), controllers ([01]) / ([01]), "
    r"created (\d+) / (\d+), metal ([\d.]+) / ([\d.]+), energy ([\d.]+) / ([\d.]+)"
)
DEMO_RESULT = re.compile(
    r"demo benchmark: (\d+) ticks in ([\d.]+) s \(([\d.]+) ticks/s\), drawing ([01])"
)
DEMO_FINISHED = re.compile(r"demo tick \d+: digest ([0-9a-f]{16}), .*frames (\d+)/(\d+), finished")


def file_hash(path):
    """Return a file's SHA-256 without loading it all into memory."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command, folder, timeout):
    """Retain the complete child log and bound the subprocess lifetime."""
    environment = os.environ.copy()
    environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    result = {"command": command, "log": str(folder / "process.log")}
    started = time.perf_counter()
    with (folder / "process.log").open("w", encoding="utf-8") as output:
        try:
            child = subprocess.run(command, cwd=folder, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, timeout=timeout, check=False)
            result["returncode"] = child.returncode
            if child.returncode != 0:
                result["error"] = f"process exited with {child.returncode}"
        except (OSError, subprocess.TimeoutExpired) as error:
            result["error"] = str(error)
    result["process_wall_seconds"] = time.perf_counter() - started
    return result, (folder / "process.log").read_text(encoding="utf-8", errors="replace")


def parse_ai(result, output, ticks):
    """Validate synchronized native advancement and retain workload windows."""
    matches = AI_RESULT.findall(output)
    if len(matches) != 1:
        result.setdefault("error", f"expected one AI result, found {len(matches)}")
        return
    host_ticks, client_ticks, seconds, rate, _, peak, defeated, host_errors, client_errors = matches[0]
    host_ticks, client_ticks = int(host_ticks), int(client_ticks)
    seconds, rate = float(seconds), float(rate)
    defeated = defeated == "1"
    result.update(ticks=host_ticks, peer_ticks=client_ticks, engine_loop_seconds=seconds,
                  engine_loop_ticks_per_second=rate, normal_speed_multiple=rate / NORMAL_TICKS_PER_SECOND,
                  peak_units=int(peak), defeated=defeated)
    result["windows"] = [
        {"tick": int(tick), "units": [int(first), int(second)],
         "kills": [int(first_kills), int(second_kills)], "ticks_per_second": float(window_rate)}
        for tick, first, second, first_kills, second_kills, window_rate in AI_WINDOW.findall(output)
    ]
    states = {int(tick): {"commander_rule": int(rule),
                         "participating": [int(first), int(second)],
                         "controllers": [int(first_controller), int(second_controller)],
                         "units_created": [int(first_created), int(second_created)],
                         "metal_produced_total": [float(first_metal), float(second_metal)],
                         "energy_produced_total": [float(first_energy), float(second_energy)]}
              for (tick, rule, first, second, first_controller, second_controller,
                   first_created, second_created, first_metal, second_metal,
                   first_energy, second_energy) in AI_STATE.findall(output)}
    for window in result["windows"]:
        window.update(states.get(window["tick"], {}))
    if any(window.get("commander_rule") != 0 or "controllers" not in window for window in result["windows"]):
        result.setdefault("error", "AI windows lack annihilation rules or controller state")
    if (host_ticks <= 0 or host_ticks != client_ticks or host_ticks > ticks or
            (host_ticks != ticks and not defeated) or seconds <= 0 or rate <= 0 or
            int(host_errors) != 0 or int(client_errors) != 0 or not result["windows"]):
        result.setdefault("error", "AI result has invalid progress, timing, windows or record errors")


def compare_recording(recorded, unrecorded):
    """Compare paired costs only after checking equal simulated progress."""
    result = {}
    if "error" in recorded or "error" in unrecorded or recorded.get("ticks") != unrecorded.get("ticks"):
        return {"error": "recording comparison needs valid runs with equal tick counts"}
    result["observed_loop_seconds_ratio"] = (
        recorded["engine_loop_seconds"] / unrecorded["engine_loop_seconds"])
    result["observed_loop_overhead_percent"] = 100 * (result["observed_loop_seconds_ratio"] - 1)
    result["observed_process_wall_seconds_ratio"] = (
        recorded["process_wall_seconds"] / unrecorded["process_wall_seconds"])
    counters = ("units", "kills", "units_created", "metal_produced_total", "energy_produced_total")
    first, second = recorded["windows"][-1], unrecorded["windows"][-1]
    result["final_reported_counters_equal"] = all(first.get(key) == second.get(key) for key in counters)
    if not result["final_reported_counters_equal"]:
        result["error"] = "paired runs ended with different reported workload counters"
    result["note"] = "Ratios are observed paired timings. Process wall includes startup, loading, " \
                     "recording serialization and exit; it does not isolate serialization. " \
                     "Matching summary counters do not establish full simulation equivalence."
    return result


def parse_replay(result, output):
    """Require a fully consumed, successful replay without drawn frames."""
    matches = DEMO_RESULT.findall(output)
    finished = DEMO_FINISHED.findall(output)
    if len(matches) != 1 or len(finished) != 1:
        result.setdefault("error", "expected one replay timing result and a finished recording")
        return
    ticks, seconds, rate, drawing = matches[0]
    digest, delivered, frames = finished[0]
    ticks, seconds, rate = int(ticks), float(seconds), float(rate)
    result.update(ticks=ticks, engine_loop_seconds=seconds, engine_loop_ticks_per_second=rate,
                  normal_speed_multiple=rate / NORMAL_TICKS_PER_SECOND, world_digest=digest,
                  frames_delivered=int(delivered), recording_frames=int(frames))
    if ticks <= 0 or seconds <= 0 or rate <= 0 or drawing != "0" or delivered != frames:
        result.setdefault("error", "replay result has invalid progress, timing or drawing enabled")


def benchmark(args):
    """Record independent opposing AI matches, or replay the provided recording."""
    native, game_dir = args.native.resolve(), args.game_dir.resolve()
    if not native.is_file() or not game_dir.is_dir():
        raise ValueError("--native must name a file and --game-dir must name a directory")
    existing = args.recording.resolve() if args.recording else None
    if existing is not None and not existing.is_file():
        raise ValueError("--recording must name an existing .tad file")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="ai-benchmark-", dir=args.scratch_root.resolve()))
    report = {
        "schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
        "native": {"path": str(native), "sha256": file_hash(native)},
        "game_dir": str(game_dir), "ticks_limit": args.ticks, "seed": args.seed,
        "normal_ticks_per_second": NORMAL_TICKS_PER_SECOND, "target_speed_multiple": args.goal_speed,
        "live_human_participant_required": False if existing is None else None,
        "scratch": str(folder), "compare_no_record": args.compare_no_record, "runs": [],
        "timing_note": "Live AI loop includes two matches, networking and reporting; setup and "
                       "recording serialization are excluded. Replay executes recorded decisions "
                       "without live AI. Process wall time includes loading and exit.",
    }
    for repeat in range(args.repeats):
        recording = existing or folder / f"match-{repeat + 1}.tad"
        entry = {"recording": str(recording)}
        common = [str(native), "--game-dir", str(game_dir), "--headless-check", "--skip-intro", "--mute"]
        if existing is None:
            # Alternate pair order to reduce a systematic warm-cache/order bias.
            modes = ["unrecorded", "recorded"] if args.compare_no_record else ["recorded"]
            if args.compare_no_record and repeat % 2 != 0:
                modes.reverse()
            entry["live_order"] = modes
            for mode in modes:
                live_folder = folder / f"live-{mode}-{repeat + 1}"
                live_folder.mkdir()
                command = [*common, "--net-ai-benchmark", str(args.ticks), "--seed", str(args.seed),
                           "--preferences-file", str(live_folder / "preferences.conf")]
                if mode == "recorded":
                    command += ["--net-record", str(recording)]
                live, output = run(command, live_folder, args.timeout)
                parse_ai(live, output, args.ticks)
                live["recording_enabled"] = mode == "recorded"
                if mode == "unrecorded" and "multiplayer: recorded the game to " in output:
                    live.setdefault("error", "the profile automatically recorded the nominally unrecorded run")
                live["target_met"] = ("error" not in live and
                                      live["normal_speed_multiple"] >= args.goal_speed)
                if live.get("windows"):
                    live["slowest_window_speed_multiple"] = min(
                        window["ticks_per_second"] for window in live["windows"]) / NORMAL_TICKS_PER_SECOND
                entry["live_ai" if mode == "recorded" else "unrecorded_live_ai"] = live
            if args.compare_no_record:
                entry["recording_comparison"] = compare_recording(
                    entry["live_ai"], entry["unrecorded_live_ai"])
        if recording.is_file():
            entry["recording_sha256"] = file_hash(recording)
            entry["recording_bytes"] = recording.stat().st_size
            replay_folder = folder / f"replay-{repeat + 1}"
            replay_folder.mkdir()
            command = [*common, "--play-demo", str(recording), "--match-ticks", str(args.ticks + 1000),
                       "--preferences-file", str(replay_folder / "preferences.conf")]
            replay, output = run(command, replay_folder, args.timeout)
            parse_replay(replay, output)
            entry["replay"] = replay
        else:
            entry["error"] = "recording was not written"
        report["runs"].append(entry)
    report["ok"] = all("error" not in entry and "error" not in entry.get("live_ai", {}) and
                       "error" not in entry.get("replay", {}) and
                       "error" not in entry.get("unrecorded_live_ai", {}) and
                       "error" not in entry.get("recording_comparison", {}) for entry in report["runs"])
    return report


def self_test():
    """Check reporting rejects absent, partial and invalid native results."""
    good = "AI benchmark: 36000 / 36000 ticks in 30.000000 s (1200.00 ticks/s; 40.00x normal), peak units 160, defeated 0, record errors 0 / 0\nAI window: tick 1800, units 4 / 3, kills 0 / 0, 1200.00 ticks/s\nAI state: tick 1800, rules 0, participating 1 / 1, controllers 1 / 1, created 4 / 3, metal 100 / 120, energy 500 / 600"
    result = {}
    parse_ai(result, good, 36000)
    if "error" in result or result.get("normal_speed_multiple") != 40:
        raise ValueError("valid AI result was rejected")
    for output in ("", good.replace("36000 / 36000", "36000 / 35999"),
                   good.replace("errors 0 / 0", "errors 1 / 0")):
        result = {}
        parse_ai(result, output, 36000)
        if "error" not in result:
            raise ValueError("invalid AI result was accepted")
    replay = "demo benchmark: 36000 ticks in 10.000000 s (3600.00 ticks/s), drawing 0\ndemo tick 36000: digest 0123456789abcdef, 160 live units, frames 100/100, finished"
    result = {}
    parse_replay(result, replay)
    if "error" in result or result.get("world_digest") != "0123456789abcdef":
        raise ValueError("valid replay result was rejected")
    for output in ("", replay.replace(", finished", ""), replay.replace("drawing 0", "drawing 1")):
        result = {}
        parse_replay(result, output)
        if "error" not in result:
            raise ValueError("invalid replay result was accepted")
    recorded = {"ticks": 100, "engine_loop_seconds": 2, "process_wall_seconds": 4,
                "windows": [{"units": [1, 2], "kills": [0, 0]}]}
    unrecorded = dict(recorded, engine_loop_seconds=1, process_wall_seconds=2)
    comparison = compare_recording(recorded, unrecorded)
    if comparison.get("observed_loop_overhead_percent") != 100 or not comparison.get("final_reported_counters_equal"):
        raise ValueError("recording comparison did not retain the paired timings")
    if "error" not in compare_recording(recorded, dict(unrecorded, ticks=90)):
        raise ValueError("unequal tick counts were accepted for recording comparison")
    print("AI benchmark self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--recording", type=Path)
    parser.add_argument("--compare-no-record", action="store_true",
                        help="pair each live recorded match with the same seed/ticks without recording")
    parser.add_argument("--ticks", type=int, default=36000)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--goal-speed", type=float, default=500)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.recording and args.compare_no_record:
        parser.error("--compare-no-record needs live AI matches; omit --recording")
    if not all((args.native, args.game_dir, args.scratch_root)):
        parser.error("--native, --game-dir and --scratch-root are required")
    if (args.ticks <= 0 or args.repeats <= 0 or not 0 <= args.seed <= 0xffffffff or
            not math.isfinite(args.goal_speed) or args.goal_speed <= 0 or
            not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("ticks/repeats/goal-speed/timeout must be positive and seed must be a uint32")
    report = benchmark(args)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
