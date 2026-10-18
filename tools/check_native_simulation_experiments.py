#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check repeatable local AI play for all optional simulation modes.

Each combination runs twice from one seed. Explicitly disabling both options
must reproduce the default state. Modes may differ from each other. Optional
full trace comparisons run separately from measured repetitions.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import sys
import tempfile

from benchmark_local_ai import benchmark, check_trace, files_equal, require, run_native, simulation_signature


def check_off_switch(default, explicit):
    """Require the explicit off settings to reproduce the default simulation."""
    require("error" not in default and "error" not in explicit,
            "off-switch comparison needs two valid native runs")
    require(simulation_signature(default) == simulation_signature(explicit),
            "explicit off settings changed the default simulation")


def check_modes(args):
    """Exercise every mode combination and retain their independent reports."""
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="simulation-experiments-", dir=args.scratch_root.resolve()))
    report = {"schema_version": 1, "scratch": str(folder), "modes": [], "errors": [],
              "comparison_note": "Each mode must repeat its own seeded state. Arithmetic and "
              "route changes may alter matches, so cross-mode digests need not agree. "
              "Only explicit off settings must agree with the default mode."}
    default = None
    for math_mode in ("off", "hardware"):
        for pathfinder in ("compatible", "weighted"):
            mode_args = argparse.Namespace(**vars(args), repeats=2, goal_speed=500,
                                           warmup_ticks=18000, experimental_math=math_mode,
                                           experimental_pathfinder=pathfinder)
            mode_args.scratch_root = folder
            measured = benchmark(mode_args, require_activity=True)
            report["modes"].append({"experimental_math": math_mode,
                                    "experimental_pathfinder": pathfinder, "report": measured})
            if not measured["ok"]:
                report["errors"].append(f"{math_mode}/{pathfinder} failed native validation")
            if math_mode == "off" and pathfinder == "compatible":
                default = measured
    if default and default["ok"]:
        explicit_args = argparse.Namespace(**vars(args), experimental_math="off",
                                           experimental_pathfinder="compatible",
                                           explicit_compatible_modes=True)
        explicit_args.native = args.native.resolve()
        explicit_args.game_dir = args.game_dir.resolve()
        explicit = run_native(explicit_args, folder / "explicit-off", trace=args.validate_traces)
        report["explicit_off"] = explicit
        try:
            check_off_switch(default["runs"][0], explicit)
            report["default_and_explicit_off_equal"] = True
            if args.validate_traces:
                explicit_path = folder / "explicit-off" / "world.trace"
                explicit["trace"] = check_trace(explicit_path, explicit["summary"], args.seed)
                default_path = Path(default["trace_runs"][0]["trace"]["path"])
                require(files_equal(default_path, explicit_path),
                        "explicit off settings changed the default full trace")
                report["default_and_explicit_off_traces_equal"] = True
        except (OSError, ValueError, KeyError) as error:
            report["errors"].append(str(error))
    report["ok"] = not report["errors"] and bool(report.get("default_and_explicit_off_equal"))
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def self_test():
    """Check that off-switch validation ignores timing and rejects state drift."""
    state = {"tick": 1800, "advanced_ticks": 1800, "units": [4, 5], "kills": [0, 0],
             "participating": [True, True], "controllers": [True, True], "created": [4, 5],
             "metal": [100, 110], "energy": [1000, 1100]}
    reference = {"config": {"math_mode": "compatible", "pathfinder": "compatible"},
                 "windows": [state], "summary": {**state, "requested_ticks": 1800,
                 "start_tick": 0, "peak_units": 9, "natural_end": False, "outcome": "ongoing",
                 "digest": "0123456789abcdef", "seconds": 1.0}}
    explicit = copy.deepcopy(reference)
    explicit["summary"]["seconds"] = 2.0
    check_off_switch(reference, explicit)
    for field, value in (("digest", "abcdef0123456789"), ("units", [3, 5])):
        invalid = copy.deepcopy(explicit)
        invalid["summary"][field] = value
        try:
            check_off_switch(reference, invalid)
        except ValueError:
            pass
        else:
            raise ValueError(f"off-switch validation ignored changed {field}")
    print("simulation experiments self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--ticks", type=int, default=1800)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--map")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--validate-traces", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not all((args.native, args.game_dir, args.scratch_root)):
        parser.error("--native, --game-dir and --scratch-root are required")
    if (args.ticks <= 0 or not 0 <= args.seed <= 0xffffffff or
            not math.isfinite(args.timeout) or args.timeout <= 0 or
            (args.map is not None and not args.map.strip())):
        parser.error("ticks and timeout must be positive; seed must be uint32; map must not be blank")
    report = check_modes(args)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
