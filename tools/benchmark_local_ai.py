#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Measure two opposing native computer players in one local match.

JSON goes on stdout. Full commands, logs and optional per-tick traces remain
under --scratch-root. Trace validation runs separately from timed repetitions.
No speed threshold makes this correctness check fail.
"""
import argparse
import copy
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import statistics
import struct
import subprocess
import sys
import tempfile
import time

PREFIX = "local-ai: "
NORMAL_TICKS_PER_SECOND = 30
STATE_FIELDS = ("units", "kills", "participating", "controllers", "created", "metal", "energy")
TRACE_HEADER_BYTES = 16
TRACE_RECORD_BYTES = 24
TRACE_SECTIONS = 7
TRACE_TICK_BYTES = TRACE_RECORD_BYTES * (1 + TRACE_SECTIONS)


def file_hash(path):
    """Return a file's SHA-256 without loading it all into memory."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    """Reject incomplete or inconsistent native reports."""
    if not condition:
        raise ValueError(message)


def number(value, minimum=0):
    """Check a JSON number is finite and within its permitted range."""
    return type(value) in (int, float) and math.isfinite(value) and value >= minimum


def integer(value, minimum=0):
    """Check a JSON integer without accepting a boolean as a count."""
    return type(value) is int and value >= minimum


def state_valid(state):
    """Check the two computer players' workload counters."""
    for key in STATE_FIELDS:
        values = state.get(key)
        if not isinstance(values, list) or len(values) != 2:
            return False
        if key in ("participating", "controllers"):
            if not all(type(value) is bool for value in values):
                return False
        elif key in ("metal", "energy"):
            if not all(number(value) for value in values):
                return False
        elif not all(integer(value) for value in values):
            return False
    return True


def parse_output(output, ticks, seed, map_name=None, math_mode="compatible"):
    """Require complete local AI configuration, progress and final state."""
    records = []
    for line in output.splitlines():
        if line.startswith(PREFIX):
            record = json.loads(line[len(PREFIX):])
            require(isinstance(record, dict), "local AI record is not an object")
            records.append(record)
    require(len(records) >= 3 and records[0].get("type") == "config" and
            records[-1].get("type") == "summary", "missing local AI configuration or summary")
    config, summary = records[0], records[-1]
    windows = records[1:-1]
    require(all(window.get("type") == "window" for window in windows),
            "unexpected or duplicate local AI record")
    require(config.get("requested_ticks") == ticks and config.get("seed") == seed and
            config.get("players") == [0, 1] and config.get("sides") == ["Arm", "Core"] and
            config.get("difficulty") == "hard" and config.get("unit_limit") == 250 and
            integer(config.get("unit_slots"), 501) and config.get("session_rules") == 0 and
            config.get("normal_ticks_per_second") == NORMAL_TICKS_PER_SECOND and
            config.get("math_mode", "compatible") == math_mode and
            config.get("pathfinder", "compatible") == "compatible" and
            integer(config.get("start_tick")) and isinstance(config.get("map"), str) and
            bool(config["map"]), "invalid local AI configuration")
    if map_name is not None:
        require(config["map"].casefold() == map_name.casefold(), "requested map was not selected")
    require(summary.get("requested_ticks") == ticks and
            summary.get("start_tick") == config["start_tick"] and
            integer(summary.get("advanced_ticks"), 1) and summary["advanced_ticks"] <= ticks and
            integer(summary.get("tick")) and
            summary["tick"] == config["start_tick"] + summary["advanced_ticks"] and
            type(summary.get("natural_end")) is bool and
            (summary["advanced_ticks"] == ticks or summary["natural_end"]) and
            isinstance(summary.get("outcome"), str) and bool(summary["outcome"]) and
            isinstance(summary.get("digest"), str) and
            re.fullmatch(r"[0-9a-f]{16}", summary["digest"]) is not None and
            integer(summary.get("peak_units"), 2), "invalid local AI final progress or digest")
    previous = 0
    previous_state = None
    for index, record in enumerate([*windows, summary]):
        require(state_valid(record), "invalid local AI workload counters")
        advanced = record.get("advanced_ticks")
        if index < len(windows):
            require(integer(advanced, 1) and previous < advanced <= summary["advanced_ticks"] and
                    record.get("tick") == config["start_tick"] + advanced,
                    "local AI windows do not advance consecutively")
            require(record.get("window_ticks") == advanced - previous,
                    "local AI window interval does not match actual advancement")
            previous = advanced
        require(number(record.get("seconds")) and record["seconds"] > 0 and
                number(record.get("ticks_per_second")) and record["ticks_per_second"] > 0 and
                number(record.get("normal_speed")) and record["normal_speed"] > 0,
                "invalid local AI timing")
        require(math.isclose(record["normal_speed"], record["ticks_per_second"] /
                             NORMAL_TICKS_PER_SECOND, rel_tol=0.01, abs_tol=0.01),
                "local AI speed multiple does not match its tick rate")
        final = record["tick"] == summary["tick"] and summary["natural_end"]
        require(final or (all(record["controllers"]) and all(record["participating"]) and
                          min(record["units"]) > 0), "local AI stopped participating before the end")
        if previous_state is not None:
            require(all(record[key][player] >= previous_state[key][player]
                        for key in ("kills", "created", "metal", "energy") for player in (0, 1)),
                    "local AI cumulative counters moved backwards")
        previous_state = record
    require(windows[-1]["tick"] == summary["tick"], "final local AI window is missing")
    require(all(windows[-1][key] == summary[key] for key in STATE_FIELDS),
            "final local AI window and summary disagree")
    return {"config": config, "windows": windows, "summary": summary}


def simulation_signature(run):
    """Select configuration and simulation results without measured timing."""
    def state(record):
        return {key: record[key] for key in ("tick", "advanced_ticks", *STATE_FIELDS)}
    summary = run["summary"]
    return {"config": run["config"], "windows": [state(window) for window in run["windows"]],
            "summary": {**state(summary), **{key: summary[key] for key in
                        ("requested_ticks", "start_tick", "peak_units", "natural_end", "outcome", "digest")}}}


def run_native(args, folder, trace=False):
    """Retain a bounded native process's command, output and parsed results."""
    folder.mkdir()
    command = [*shlex.split(os.environ.get("OA_TEST_RUNNER", "")), str(args.native),
               "--game-dir", str(args.game_dir), "--headless-check", "--skip-intro", "--mute",
               "--local-ai-benchmark", str(args.ticks), "--seed", str(args.seed),
               "--preferences-file", str(folder / "preferences.conf")]
    if args.map:
        command += ["--local-ai-map", args.map]
    experimental_math = getattr(args, "experimental_math", "off")
    if experimental_math != "off" or getattr(args, "explicit_compatible_math", False):
        command += ["--experimental-math", experimental_math]
    if trace:
        command += ["--trace-digest", str(folder / "world.trace")]
    result = {"command": command, "log": str(folder / "process.log"), "trace_enabled": trace}
    environment = os.environ.copy()
    environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                       SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    started = time.perf_counter()
    with (folder / "process.log").open("w", encoding="utf-8") as output:
        try:
            child = subprocess.run(command, cwd=folder, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, timeout=args.timeout, check=False)
            result["returncode"] = child.returncode
            require(child.returncode == 0, f"process exited with {child.returncode}")
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            result["error"] = str(error)
    result["process_wall_seconds"] = time.perf_counter() - started
    if "error" not in result:
        try:
            result.update(parse_output((folder / "process.log").read_text(
                encoding="utf-8", errors="replace"), args.ticks, args.seed, args.map,
                "hardware" if experimental_math == "hardware" else "compatible"))
        except (ValueError, KeyError, TypeError) as error:
            result["error"] = str(error)
    return result


def check_trace(path, summary, seed):
    """Require one complete ordered digest record set for every advanced tick."""
    ticks = summary["advanced_ticks"]
    require(path.stat().st_size == TRACE_HEADER_BYTES + TRACE_TICK_BYTES * ticks,
            "local AI trace length does not match actual tick advancement")
    with path.open("rb") as source:
        require(struct.unpack("<4I", source.read(TRACE_HEADER_BYTES)) ==
                (0x3154414F, 1, TRACE_RECORD_BYTES, seed), "invalid local AI trace header")
        for tick in range(summary["start_tick"] + 1, summary["tick"] + 1):
            record = struct.unpack("<6I", source.read(TRACE_RECORD_BYTES))
            require(record[:2] == (1, tick), "local AI trace tick is missing or out of order")
            for section in range(TRACE_SECTIONS):
                record = struct.unpack("<6I", source.read(TRACE_RECORD_BYTES))
                require(record[:3] == (0x10, tick, section), "local AI trace section is out of order")
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": file_hash(path), "ticks": ticks}


def files_equal(first, second):
    """Compare complete trace files in bounded blocks."""
    with first.open("rb") as left, second.open("rb") as right:
        while True:
            block = left.read(1024 * 1024)
            if block != right.read(1024 * 1024):
                return False
            if not block:
                return True


def timing_statistics(runs, warmup_ticks, goal_speed):
    """Report total and developed-window speeds without a pass threshold."""
    valid = [run for run in runs if "error" not in run]
    if not valid:
        return {}
    rates = [run["summary"]["ticks_per_second"] for run in valid]
    developed = []
    all_windows = []
    for run in valid:
        previous = 0
        for window in run["windows"]:
            sample = {"start_advanced_tick": previous, **window}
            all_windows.append(sample)
            if previous >= warmup_ticks:
                developed.append(sample)
            previous = window["advanced_ticks"]
    result = {"median_ticks_per_second": statistics.median(rates),
              "median_normal_speed": statistics.median(rates) / NORMAL_TICKS_PER_SECOND,
              "minimum_ticks_per_second": min(rates), "maximum_ticks_per_second": max(rates),
              "aggregate_ticks_per_second": sum(run["summary"]["advanced_ticks"] for run in valid) /
              sum(run["summary"]["seconds"] for run in valid),
              "slowest_window_normal_speed": min(window["ticks_per_second"] for window in all_windows) /
              NORMAL_TICKS_PER_SECOND, "developed_windows_after_ticks": warmup_ticks,
              "developed_window_count": len(developed), "goal_normal_speed": goal_speed}
    result["median_goal_met"] = result["median_normal_speed"] >= goal_speed
    result["aggregate_normal_speed"] = result["aggregate_ticks_per_second"] / NORMAL_TICKS_PER_SECOND
    if developed:
        result["slowest_developed_window_normal_speed"] = min(
            window["ticks_per_second"] for window in developed) / NORMAL_TICKS_PER_SECOND
        result["median_developed_window_normal_speed"] = statistics.median(
            window["ticks_per_second"] for window in developed) / NORMAL_TICKS_PER_SECOND
    return result


def benchmark(args, require_activity=False):
    """Measure independent matches and optionally validate separate full traces."""
    args.native, args.game_dir = args.native.resolve(), args.game_dir.resolve()
    require(args.native.is_file() and args.game_dir.is_dir(),
            "--native must name a file and --game-dir must name a directory")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="local-ai-benchmark-", dir=args.scratch_root.resolve()))
    report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
              "native": {"path": str(args.native), "sha256": file_hash(args.native)},
              "game_dir": str(args.game_dir), "seed": args.seed, "ticks_limit": args.ticks,
              "experimental_math": getattr(args, "experimental_math", "off"),
              "experimental_pathfinder": "compatible",
              "requested_map": args.map, "normal_ticks_per_second": NORMAL_TICKS_PER_SECOND,
              "scratch": str(folder), "runs": [], "errors": [],
              "timing_note": "One match with two live local AI controllers; no networking, drawing "
              "or wall-clock pacing. Native loop includes periodic reporting; setup is excluded. "
              "Process wall time includes loading and exit. Trace checks are separate runs."}
    for repeat in range(args.repeats):
        report["runs"].append(run_native(args, folder / f"timed-{repeat + 1}"))
    valid = [run for run in report["runs"] if "error" not in run]
    if valid:
        reference = simulation_signature(valid[0])
        report["repeated_state_equal"] = all(simulation_signature(run) == reference for run in valid)
        if not report["repeated_state_equal"]:
            report["errors"].append("repeated seeded matches produced different states or digests")
        state = valid[0]["summary"]
        report["activity_observed"] = any(value > 1 for value in state["created"]) or any(
            value > 0 for key in ("metal", "energy") for value in state[key])
        if require_activity and not report["activity_observed"]:
            report["errors"].append("neither AI produced a unit or resource during the native check")
        if args.validate_traces:
            report["trace_runs"] = []
            trace_paths = []
            for repeat in range(2):
                trace_folder = folder / f"trace-{repeat + 1}"
                traced = run_native(args, trace_folder, trace=True)
                report["trace_runs"].append(traced)
                if "error" not in traced:
                    try:
                        require(simulation_signature(traced) == reference,
                                "tracing changed the reported simulation state")
                        trace_path = trace_folder / "world.trace"
                        traced["trace"] = check_trace(trace_path, traced["summary"], args.seed)
                        trace_paths.append(trace_path)
                    except (OSError, ValueError, struct.error) as error:
                        traced["error"] = str(error)
            report["complete_traces_equal"] = len(trace_paths) == 2 and files_equal(*trace_paths)
            if not report["complete_traces_equal"]:
                report["errors"].append("separate full trace runs are incomplete or differ")
    report["statistics"] = timing_statistics(report["runs"], args.warmup_ticks, args.goal_speed)
    report["ok"] = (len(valid) == args.repeats and not report["errors"] and
                    all("error" not in run for run in report.get("trace_runs", [])))
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def self_test():
    """Check malformed, frozen and nondeterministic report handling."""
    state = {"units": [5, 6], "kills": [0, 0], "participating": [True, True],
             "controllers": [True, True], "created": [5, 6], "metal": [50.5, 60.5],
             "energy": [100.0, 120.0]}
    config = {"type": "config", "map": "Ashap Plateau", "seed": 123, "requested_ticks": 1800,
              "start_tick": 0, "players": [0, 1], "sides": ["Arm", "Core"], "difficulty": "hard",
              "unit_limit": 250, "unit_slots": 2501, "session_rules": 0, "normal_ticks_per_second": 30,
              "math_mode": "compatible", "pathfinder": "compatible"}
    window = {"type": "window", "tick": 1800, "advanced_ticks": 1800, "seconds": 1.5,
              "window_ticks": 1800, "ticks_per_second": 1200, "normal_speed": 40, **state}
    summary = {**window, "type": "summary", "requested_ticks": 1800, "start_tick": 0,
               "peak_units": 11, "natural_end": False, "outcome": "ongoing", "digest": "0123456789abcdef"}

    def output(records):
        return "unrelated game startup\n" + "\n".join(PREFIX + json.dumps(record) for record in records)

    records = [config, window, summary]
    parsed = parse_output(output(records), 1800, 123)
    require(parsed["summary"]["digest"] == summary["digest"], "valid report was rejected")
    for math_mode in ("compatible", "hardware"):
        modes = copy.deepcopy(records)
        modes[0]["math_mode"] = math_mode
        mode_run = parse_output(output(modes), 1800, 123, math_mode=math_mode)
        require(mode_run["config"]["math_mode"] == math_mode, "requested math mode was not retained")
        if math_mode != "compatible":
            try:
                parse_output(output(modes), 1800, 123)
            except ValueError:
                pass
            else:
                raise ValueError("unexpected experimental math mode was accepted")
    absent = copy.deepcopy(records)
    del absent[0]["math_mode"]
    parse_output(output(absent), 1800, 123)
    try:
        parse_output(output(absent), 1800, 123, math_mode="hardware")
    except ValueError:
        pass
    else:
        raise ValueError("a missing math mode field was accepted for an experimental mode")
    unsupported_pathfinder = copy.deepcopy(records)
    unsupported_pathfinder[0]["pathfinder"] = "unsupported"
    try:
        parse_output(output(unsupported_pathfinder), 1800, 123)
    except ValueError:
        pass
    else:
        raise ValueError("a removed pathfinder mode was accepted")
    for field, value in (("advanced_ticks", 1799), ("tick", 1801), ("seconds", 0),
                         ("ticks_per_second", float("nan")), ("units", [5]),
                         ("controllers", [False, True]), ("digest", "bad"),
                         ("created", [True, 6])):
        bad = copy.deepcopy(records)
        bad[-1][field] = value
        try:
            parse_output(output(bad), 1800, 123)
        except ValueError:
            pass
        else:
            raise ValueError(f"invalid {field} was accepted")
    for bad in ([], records[:-1], [config, summary], [config, window, window, summary]):
        try:
            parse_output(output(bad), 1800, 123)
        except ValueError:
            pass
        else:
            raise ValueError("missing or duplicate native records were accepted")
    ended = copy.deepcopy(records)
    for record in ended[1:]:
        record.update(tick=900, advanced_ticks=900, units=[0, 6], participating=[False, True])
    ended[1]["window_ticks"] = 900
    ended[-1].update(natural_end=True, outcome="defeat")
    parse_output(output(ended), 1800, 123)
    changed = copy.deepcopy(parsed)
    changed["summary"]["digest"] = "abcdef0123456789"
    require(simulation_signature(parsed) != simulation_signature(changed), "digest change was ignored")
    faster = copy.deepcopy(parsed)
    faster["summary"]["seconds"] = 0.5
    faster["summary"]["ticks_per_second"] = 3600
    require(simulation_signature(parsed) == simulation_signature(faster), "timing changed state comparison")
    require(timing_statistics([parsed], 1800, 500)["median_normal_speed"] == 40,
            "timing statistics do not use the actual tick rate")
    with tempfile.TemporaryDirectory(prefix="local-ai-parser-") as temporary:
        path = Path(temporary) / "world.trace"
        header = struct.pack("<4I", 0x3154414F, 1, TRACE_RECORD_BYTES, 123)
        body = struct.pack("<6I", 1, 1, 2, 0, 0, 0) + b"".join(
            struct.pack("<6I", 0x10, 1, section, 0, 0, 0) for section in range(TRACE_SECTIONS))
        path.write_bytes(header + body)
        trace_summary = {"advanced_ticks": 1, "start_tick": 0, "tick": 1}
        require(check_trace(path, trace_summary, 123)["ticks"] == 1, "valid trace was rejected")
        for invalid in ((header + body)[:-1], header + bytes(TRACE_TICK_BYTES),
                        struct.pack("<4I", 0x3154414F, 1, TRACE_RECORD_BYTES, 124) + body):
            path.write_bytes(invalid)
            try:
                check_trace(path, trace_summary, 123)
            except ValueError:
                pass
            else:
                raise ValueError("partial, unordered or wrong-seed trace was accepted")
    print("local AI benchmark self-test passed")


def main(require_activity=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--ticks", type=int, default=36000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--map", help="installed skirmish map name")
    parser.add_argument("--experimental-math", choices=("off", "hardware"), default="off",
                        help="select compatible arithmetic or offline hardware rotation arithmetic")
    parser.add_argument("--warmup-ticks", type=int, default=18000,
                        help="report developed windows starting after this many simulated ticks")
    parser.add_argument("--goal-speed", type=float, default=500)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--validate-traces", action="store_true",
                        help="compare two additional full per-tick traces outside measured repetitions")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not all((args.native, args.game_dir, args.scratch_root)):
        parser.error("--native, --game-dir and --scratch-root are required")
    if (args.ticks <= 0 or args.repeats <= 0 or args.warmup_ticks < 0 or
            not 0 <= args.seed <= 0xffffffff or not math.isfinite(args.goal_speed) or
            args.goal_speed <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0 or
            (args.map is not None and not args.map.strip())):
        parser.error("ticks/repeats/goal-speed/timeout must be positive; warmup nonnegative; seed uint32")
    report = benchmark(args, require_activity=require_activity)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
