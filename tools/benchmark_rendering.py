#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Compare undrawn and rendered native matches without opening a visible window.

Headless cases compose real battlefield pixels on the CPU. --presentation also
exercises SDL upload/presentation through its offscreen driver; the engine may
fall back from the requested Full tier. This is not a physical-GPU benchmark.
Frame-rate options are frames per simulated second. The wall-clock suite uses
--max-fps to cap drawing by elapsed real time while simulation runs freely.
All captures, logs and fresh preferences stay under --scratch-root.
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
import statistics
import subprocess
import sys
import tempfile
import time

TICKS_PER_SECOND = 30
SIMULATION_LINE = re.compile(
    r"simulation run: (\d+) ticks in ([\d.]+) s .*world digest ([0-9a-fA-F]{16})")
FRAME_LINE = re.compile(
    r"frame run: (\d+) frames a second, (\d+) frames, (\d+) ticks, .*"
    r"world digest ([0-9a-fA-F]{16}), frames digest ([0-9a-fA-F]{16})")
# Newer engines report exact loop time for the two rendered headless modes.
RENDER_TIME_LINE = re.compile(
    r"render run: (\d+) ticks, (\d+) frames in ([\d.]+) s .*"
    r"world digest ([0-9a-fA-F]{16})")
BENCHMARK_LINE = re.compile(
    r"benchmark (.+?)\s+(\d+) frames @ (\d+)x(\d+) zoom ([\d.]+): "
    r"([\d.]+) ms/frame \(([\d.]+) fps\) \| sim ([\d.]+) \| "
    r"world ([\d.]+) \(fog ([\d.]+)\) \| hud ([\d.]+) \| "
    r"upload ([\d.]+) \| present ([\d.]+)")


def file_hash(path):
    """Hash a file in bounded chunks."""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def summarize_output(output, expected_ticks):
    """Read native timing and state reports, rejecting an incorrect tick count."""
    result = {}
    simulation = SIMULATION_LINE.search(output)
    frame = FRAME_LINE.search(output)
    rendered_time = RENDER_TIME_LINE.search(output)
    if simulation:
        result.update(ticks=int(simulation[1]), engine_loop_seconds=float(simulation[2]),
                      world_digest=simulation[3].lower(), frames=0)
    elif rendered_time:
        result.update(ticks=int(rendered_time[1]), frames=int(rendered_time[2]),
                      engine_loop_seconds=float(rendered_time[3]),
                      world_digest=rendered_time[4].lower())
        if frame:
            if (int(frame[2]), int(frame[3]), frame[4].lower()) != (
                    result["frames"], result["ticks"], result["world_digest"]):
                raise ValueError("render timing and frame summaries disagree")
            result["frames_per_simulated_second"] = int(frame[1])
            result["frames_digest"] = frame[5].lower()
    elif frame:
        result.update(frames_per_simulated_second=int(frame[1]), frames=int(frame[2]),
                      ticks=int(frame[3]), world_digest=frame[4].lower(),
                      frames_digest=frame[5].lower())
    else:
        # Older engines do not report a digest for the one-frame-per-tick path.
        ticks = re.findall(r"^tick\s+(\d+):", output, re.MULTILINE)
        if not ticks:
            raise ValueError("no native completed-tick report")
        result.update(ticks=int(ticks[-1]), frames=int(ticks[-1]))
    if result["ticks"] != expected_ticks:
        raise ValueError(f"expected {expected_ticks} ticks, got {result['ticks']}")
    if "engine_loop_seconds" in result:
        seconds = result["engine_loop_seconds"]
        if not math.isfinite(seconds) or seconds <= 0:
            raise ValueError("native loop time must be finite and positive")
        result["engine_loop_ticks_per_second"] = result["ticks"] / seconds
        result["engine_loop_normal_speed_multiple"] = result["ticks"] / seconds / TICKS_PER_SECOND
        result["engine_loop_frames_per_second"] = result["frames"] / seconds
    if re.search(r"(?:match tick|benchmark tick|simulation tick) stopped", output):
        raise ValueError("native run reported a stopped simulation")
    return result


def run_process(command, folder, timeout, presentation=False, opengl_library=None):
    """Run with a bounded deadline and retain combined diagnostics outside Git."""
    environment = os.environ.copy()
    environment.update(SDL_VIDEO_DRIVER="offscreen" if presentation else "dummy",
                       SDL_VIDEODRIVER="offscreen" if presentation else "dummy",
                       SDL_AUDIO_DRIVER="dummy", SDL_AUDIODRIVER="dummy")
    if presentation:
        environment["SDL_RENDER_DRIVER"] = "opengles2"
        if opengl_library:
            environment["SDL_OPENGL_LIBRARY"] = opengl_library
    record = {"command": command, "log": str(folder / "process.log")}
    started = time.perf_counter()
    with (folder / "process.log").open("w", encoding="utf-8") as output:
        try:
            child = subprocess.run(command, cwd=folder, env=environment, stdout=output,
                                   stderr=subprocess.STDOUT, timeout=timeout, check=False)
            record["returncode"] = child.returncode
            if child.returncode:
                record["error"] = f"process exited with {child.returncode}"
        except (subprocess.TimeoutExpired, OSError) as error:
            record["error"] = str(error)
    record["process_wall_seconds"] = time.perf_counter() - started
    output = (folder / "process.log").read_text(encoding="utf-8", errors="replace")
    record["graphics_diagnostics"] = [line for line in output.splitlines()
                                      if "open-annihilation: graphics:" in line]
    return record, output


def preferences(path, aa):
    """Write controlled visual settings in a fresh preference file."""
    values = {"open-annihilation.anti-aliasing": str(aa),
              "open-annihilation.vertical-sync": "0", "open-annihilation.max-fps": "0",
              "open-annihilation.smooth-motion": "1", "open-annihilation.explosion-flash": "full",
              "open-annihilation.zoomed-out-units": "rendered"}
    path.write_text("open-annihilation-preferences 1\n" + "".join(
        f'"{key}" "{value}"\n' for key, value in values.items()), encoding="utf-8")


def run_case(args, case, folder, validation=False):
    """Run one fixed workload with a chosen drawing path and retain a snapshot."""
    folder.mkdir()
    profile = folder / "preferences.conf"
    preferences(profile, case["anti_aliasing_samples_per_axis"])
    snapshot = folder / "final.ppm"
    command = [str(args.native), "--game-dir", str(args.game_dir), "--headless-check",
               "--skip-intro", "--mute", "--preferences-file", str(profile),
               "--match-ticks", str(args.ticks), "--seed", str(args.seed),
               "--resolution", case["resolution"], "--draw-threads", str(args.draw_threads),
               "--snapshot", str(snapshot)]
    if args.combat_units:
        command.extend(["--combat", str(args.combat_units), "--busy-combat"])
    if args.stage:
        command.extend(["--stage", str(args.stage)])
    if args.camera:
        command.extend(["--camera", args.camera])
    command.extend(case["options"])
    if validation:
        command.extend(["--trace-digest", str(folder / "world.trace")])
    record, output = run_process(command, folder, args.timeout)
    if "error" not in record:
        try:
            record.update(summarize_output(output, args.ticks))
            census = [int(value) for value in re.findall(r"\| units (\d+) projectiles", output)]
            if census:
                record["unit_census"] = {"minimum": min(census), "maximum": max(census),
                                         "last": census[-1], "samples": len(census)}
            ordered = [int(value) for value in re.findall(
                r"stage: move squad\d+: (\d+) units to", output)]
            if ordered:
                record["movement_orders"] = {"groups_ordered": len(ordered),
                    "minimum_live_units_ordered": min(ordered),
                    "maximum_live_units_ordered": max(ordered)}
            if not snapshot.is_file():
                raise ValueError("native run did not write its final snapshot")
            with snapshot.open("rb") as source:
                header = source.read(128).split()
            if len(header) < 4 or header[0] != b"P6":
                raise ValueError("invalid final snapshot PPM header")
            expected_width, expected_height = map(int, case["resolution"].split("x"))
            if (int(header[1]), int(header[2])) != (expected_width, expected_height):
                raise ValueError("snapshot size differs from requested resolution")
            record["snapshot"] = {"path": str(snapshot), "width": int(header[1]),
                                  "height": int(header[2]), "sha256": file_hash(snapshot)}
            record["process_wall_ticks_per_second"] = record["ticks"] / record["process_wall_seconds"]
            record["process_wall_normal_speed_multiple"] = (
                record["process_wall_ticks_per_second"] / TICKS_PER_SECOND)
            record["process_wall_frames_per_second"] = record["frames"] / record["process_wall_seconds"]
            if validation:
                record["trace_sha256"] = file_hash(folder / "world.trace")
        except (ValueError, OSError) as error:
            record["error"] = str(error)
    return record


def presentation_case(args, folder, forced=False):
    """Exercise offscreen SDL rendering, preserving native phase timing."""
    folder.mkdir()
    profile = folder / "preferences.conf"
    preferences(profile, 16)
    command = [str(args.native), "--game-dir", str(args.game_dir), "--skip-intro", "--mute",
               "--preferences-file", str(profile), "--benchmark", str(args.presentation_frames),
               "--resolution", "1920x1080", "--hardware-acceleration=full", "--seed", str(args.seed),
               "--max-fps", "0", "--draw-threads", str(args.draw_threads), "--snapshot", str(folder / "present.ppm")]
    if forced:
        command.append("--force-capable")
    record, output = run_process(command, folder, args.timeout, True, args.opengl_library)
    record["capability_policy_override"] = forced
    record["requested_tier"] = "full"
    record["note"] = "Offscreen SDL presentation. Inspect graphics diagnostics for actual tier; no physical GPU is assumed."
    record["actual_startup_tier"] = next((tier for tier in ("standard", "basic", "full")
        if any(f"; {tier} tier:" in line for line in record["graphics_diagnostics"])), "unknown")
    record["software_rasterizer_reported"] = any(
        "llvmpipe" in line.lower() or "softpipe" in line.lower()
        for line in record["graphics_diagnostics"])
    record["full_tier_events"] = [line for line in record["graphics_diagnostics"]
                                  if "full tier" in line.lower()]
    record["full_tier_dropped"] = any("the full tier stopped" in line
                                      for line in record["full_tier_events"])
    record["actual_final_tier"] = ("basic" if record["full_tier_dropped"]
                                   else record["actual_startup_tier"])
    record["requested_anti_aliasing_samples_per_axis"] = 16
    record["reported_full_anti_aliasing_samples_per_axis"] = sorted({
        int(match[1]) for line in record["full_tier_events"]
        for match in re.finditer(r"full tier: anti-aliasing (\d+)x\d+", line)})
    record["scenes"] = []
    for match in BENCHMARK_LINE.finditer(output):
        record["scenes"].append({"name": match[1].strip(), "frames": int(match[2]),
            "width": int(match[3]), "height": int(match[4]), "zoom": float(match[5]),
            "milliseconds_per_frame": float(match[6]), "frames_per_second": float(match[7]),
            "simulation_ms": float(match[8]), "world_ms": float(match[9]),
            "fog_ms": float(match[10]), "hud_ms": float(match[11]),
            "upload_ms": float(match[12]), "present_ms": float(match[13])})
    if "error" not in record and len(record["scenes"]) != 4:
        record["error"] = f"expected four presented benchmark scenes, got {len(record['scenes'])}"
    record["snapshots"] = [{"path": str(path), "sha256": file_hash(path)}
                           for path in sorted(folder.glob("*.ppm"))]
    return record


def benchmark(args):
    """Validate matching tick traces separately from repeated throughput runs."""
    args.native, args.game_dir = args.native.resolve(), args.game_dir.resolve()
    if args.stage:
        args.stage = args.stage.resolve()
        if not args.stage.is_file():
            raise ValueError("--stage must name a file")
    if not args.native.is_file() or not args.game_dir.is_dir():
        raise ValueError("--native must name a file and --game-dir a directory")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix="rendering-benchmark-", dir=args.scratch_root.resolve()))
    cases = [
        {"name": "undrawn", "resolution": "640x480", "anti_aliasing_samples_per_axis": 1,
         "options": ["--simulation-only"]},
        {"name": "draw-every-tick", "resolution": "640x480", "anti_aliasing_samples_per_axis": 1,
         "options": []},
        {"name": "smooth-60", "resolution": "640x480", "anti_aliasing_samples_per_axis": 1,
         "options": ["--frame-rate", "60"]},
        {"name": "1080p-aa4", "resolution": "1920x1080", "anti_aliasing_samples_per_axis": 4,
         "options": ["--frame-rate", "30"]},
    ]
    if args.suite == "wall-clock":
        cases = [{"name": "undrawn", "resolution": "1920x1080",
                  "anti_aliasing_samples_per_axis": 16, "options": ["--simulation-only"]},
                 {"name": "wall-clock-30", "resolution": "1920x1080",
                  "anti_aliasing_samples_per_axis": 16, "options": ["--max-fps", "30"]},
                 {"name": "wall-clock-60", "resolution": "1920x1080",
                  "anti_aliasing_samples_per_axis": 16, "options": ["--max-fps", "60"]}]
        for case in cases:
            case["anti_aliasing_samples_per_axis"] = args.wall_clock_aa
    elif args.suite == "max-quality":
        cases = [{**case, "resolution": "1920x1080",
                  "anti_aliasing_samples_per_axis": 16} for case in cases[:3]]
    elif args.max_quality:
        cases.append({"name": "1080p-aa16-smooth-60", "resolution": "1920x1080",
                      "anti_aliasing_samples_per_axis": 16, "options": ["--frame-rate", "60"]})
    report = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
              "native": {"path": str(args.native), "sha256": file_hash(args.native)},
              "host": {"platform": platform.platform(), "logical_cpus": os.cpu_count()},
              "config": {"ticks": args.ticks, "seed": args.seed, "repeats": args.repeats,
                         "combat_units_per_side": args.combat_units,
                         "draw_threads": args.draw_threads, "suite": args.suite,
                         "wall_clock_aa_samples_per_axis": args.wall_clock_aa,
                         "camera": args.camera, "scratch": str(folder)},
              "timing_note": "Headless means CPU pixel composition, no SDL presentation. Process wall includes startup/final snapshot. Engine loop excludes them where reported. Frame-mode loop includes frame hashing. Timed runs do not write traces. The frame-rate option is frames per simulated second; the wall-clock suite uses max-fps to cap by elapsed real time. Speed multiple uses 30 ticks/second.",
              "cases": []}
    if args.stage:
        report["config"]["stage"] = {"path": str(args.stage), "sha256": file_hash(args.stage)}
    reference_trace = None
    for case in cases:
        item = {**case, "runs": []}
        if args.validate_traces:
            item["validation"] = run_case(args, case, folder / (case["name"] + "-validation"), True)
            trace = item["validation"].get("trace_sha256")
            if reference_trace is None and trace:
                reference_trace = trace
            item["trace_equal"] = trace is not None and trace == reference_trace
        for repeat in range(args.repeats):
            item["runs"].append(run_case(args, case, folder / f"{case['name']}-{repeat + 1}"))
        valid = all("error" not in run for run in item["runs"])
        item["ok"] = valid and item.get("trace_equal", True)
        if valid:
            item["median_process_wall_ticks_per_second"] = statistics.median(
                run["process_wall_ticks_per_second"] for run in item["runs"])
            item["median_process_wall_normal_speed_multiple"] = (
                item["median_process_wall_ticks_per_second"] / TICKS_PER_SECOND)
            item["median_process_wall_frames_per_second"] = statistics.median(
                run["process_wall_frames_per_second"] for run in item["runs"])
            if all("engine_loop_ticks_per_second" in run for run in item["runs"]):
                item["median_engine_loop_ticks_per_second"] = statistics.median(
                    run["engine_loop_ticks_per_second"] for run in item["runs"])
                item["median_engine_loop_normal_speed_multiple"] = (
                    item["median_engine_loop_ticks_per_second"] / TICKS_PER_SECOND)
                item["median_engine_loop_frames_per_second"] = statistics.median(
                    run["engine_loop_frames_per_second"] for run in item["runs"])
            digests = [run.get("world_digest") for run in item["runs"]]
            item["repeat_world_digests_equal"] = all(digests) and len(set(digests)) == 1
            if all(digests) and not item["repeat_world_digests_equal"]:
                item["ok"] = False
        report["cases"].append(item)
    known_digests = {run["world_digest"] for item in report["cases"] for run in item["runs"]
                     if "world_digest" in run}
    report["reported_world_digests_equal"] = len(known_digests) == 1
    if args.presentation:
        report["presentation"] = presentation_case(args, folder / "presentation")
    if args.force_presentation:
        report["forced_presentation"] = presentation_case(args, folder / "forced-presentation", True)
    report["ok"] = (all(item["ok"] for item in report["cases"])
                    and report["reported_world_digests_equal"]
                    and "error" not in report.get("presentation", {})
                    and "error" not in report.get("forced_presentation", {}))
    return report


def self_test():
    """Check native report parsing, wrong tick rejection and subprocess deadline."""
    sim = "simulation run: 30 ticks in 0.010000 s (3000.00 ticks/s; logging included) world digest 0123456789abcdef"
    frame = "frame run: 60 frames a second, 60 frames, 30 ticks, 60 frames between ticks, world digest 0123456789abcdef, frames digest fedcba9876543210, drawing threads 1, drawing bands 1"
    assert summarize_output(sim, 30)["engine_loop_normal_speed_multiple"] == 100
    assert summarize_output(frame, 30)["frames"] == 60
    rendered = "render run: 30 ticks, 60 frames in 0.100000 s (300.00 ticks/s; 600.00 frames/s; logging included) world digest 0123456789abcdef"
    assert summarize_output(rendered, 30)["engine_loop_frames_per_second"] == 600
    assert summarize_output(frame + "\n" + rendered, 30)["engine_loop_ticks_per_second"] == 300
    assert summarize_output("tick    30: 2.00 ms avg", 30)["ticks"] == 30
    for text in ("no result", sim.replace("30 ticks", "29 ticks"),
                 sim.replace("0.010000", "0.000000"), frame + "\n" + rendered.replace("60 frames in", "59 frames in"), sim + "\nmatch tick stopped: bad"):
        try:
            summarize_output(text, 30)
        except ValueError:
            pass
        else:
            raise ValueError("malformed native output was accepted")
    with tempfile.TemporaryDirectory() as temporary:
        record, _ = run_process([sys.executable, "-c", "import time; time.sleep(10)"],
                                Path(temporary), 0.05)
        assert "error" in record
    print("rendering benchmark self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--game-dir", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--ticks", type=int, default=300)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--camera", help="Camera top-left map coordinates, e.g. 0,1000")
    parser.add_argument("--stage", type=Path, help="Apply the same staged units/orders in every headless case")
    parser.add_argument("--combat-units", type=int, default=32)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--draw-threads", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--suite", choices=("comparison", "max-quality", "wall-clock"), default="comparison",
                        help="max-quality compares every-tick/smooth-60 drawing; wall-clock compares 30/60 wall-clock FPS caps; both use identical 1080p settings with configurable wall-clock AA")
    parser.add_argument("--wall-clock-aa", type=int, choices=(1, 2, 4, 8, 16), default=4,
                        help="Anti-aliasing samples per axis in the wall-clock suite (default: 4)")
    parser.add_argument("--max-quality", action="store_true")
    parser.add_argument("--validate-traces", action="store_true")
    parser.add_argument("--presentation", action="store_true")
    parser.add_argument("--force-presentation", action="store_true",
                        help="Also exercise Full through the offscreen software renderer by overriding capability policy; requires an engine supporting --benchmark --force-capable")
    parser.add_argument("--presentation-frames", type=int, default=30)
    parser.add_argument("--opengl-library")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not all((args.native, args.game_dir, args.scratch_root)):
        parser.error("--native, --game-dir and --scratch-root are required")
    if (args.ticks <= 0 or args.repeats < 1 or args.combat_units < 0 or
            args.timeout <= 0 or not math.isfinite(args.timeout) or
            not 1 <= args.draw_threads <= 32 or not 0 <= args.seed <= 0xffffffff or
            args.presentation_frames <= 0):
        parser.error("ticks/repeats/timeout/presentation-frames must be positive, combat-units >= 0, draw-threads 1..32, seed uint32")
    if args.camera and not re.fullmatch(r"-?\d+,-?\d+", args.camera):
        parser.error("--camera must have integer X,Z coordinates")
    report = benchmark(args)
    print(json.dumps(report, indent=2))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        sys.exit(1)
