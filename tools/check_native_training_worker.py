#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Exercise real worker startup, stepping, rejection and seeded observations.

Requires installed game assets. It never substitutes a synthetic engine.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from training_client import TrainingClient, WorkerError


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def rejected(worker, operation, code, **fields):
    before = worker.observe()["tick"]
    try:
        worker.request(operation, **fields)
    except WorkerError as error:
        check(error.response["error"]["code"] == code, "wrong error response")
    else:
        raise RuntimeError("invalid request was accepted")
    check(worker.observe()["tick"] == before, "rejection advanced time")


def framing_checks(args):
    """Check malformed frames directly against a bounded native subprocess."""
    for oversized in (False, True):
        with tempfile.TemporaryDirectory(prefix="training-framing-", dir=args.scratch_root) as temp:
            folder = Path(temp)
            preferences = folder / "preferences.conf"
            preferences.write_text("open-annihilation-preferences 1\n", encoding="utf-8")
            command = [str(Path(args.native).resolve()), "--training-worker", "--game-dir",
                       str(Path(args.game_dir).resolve()), "--seed", "123", "--mute",
                       "--preferences-file", str(preferences)]
            payload = (b"x" * (1024 * 1024 + 1) + b"\n") if oversized else (
                b'{broken\n{"id":1,"id":2,"op":"hello","version":1}\n'
                b'{"id":3,"op":"hello","version":1}\n{"id":4,"op":"close"}\n')
            environment = os.environ.copy()
            environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
            result = subprocess.run(command, input=payload, capture_output=True,
                                    timeout=args.timeout, cwd=folder, env=environment, check=False)
            if oversized:
                check(result.returncode != 0, "oversize input did not terminate worker")
            else:
                check(result.returncode == 0, "malformed framing did not recover")
                responses = [json.loads(line) for line in result.stdout.splitlines()]
                check(len(responses) == 4, "framing produced wrong response count")
                for response in responses[:2]:
                    check(not response["ok"] and response["error"]["code"] == "invalid_request",
                          "malformed request did not produce invalid_request")
                    check(response["tick"] == 0, "malformed request advanced time")
                check(responses[2]["id"] == 3 and responses[2]["ok"], "hello failed after framing errors")


def trajectory(worker, extra_observations=False):
    hello = worker.reset()
    check(hello["version"] == 1, "wrong protocol version")
    first = worker.observe()
    check(first["tick"] == worker.observe()["tick"], "observe advanced time")
    if extra_observations:
        for _ in range(5):
            check(worker.observe()["observation"] == first["observation"],
                  "repeated observe changed observation")
    check(hello["simulation"] == {"math_mode": "compatible", "pathfinder": "compatible"},
          "default training policies were not reported")
    own_units = first["observation"]["own_units"]
    check(bool(own_units), "episode has no owned units")
    unit = own_units[0]
    before = first["tick"]
    try:
        worker.step([worker.stop("unknown-handle")], ticks=1)
    except WorkerError as error:
        check(error.response["error"]["code"] == "invalid_unit", "wrong handle rejection")
    else:
        raise RuntimeError("worker accepted an unknown handle")
    check(worker.observe()["tick"] == before, "rejected command advanced time")
    player = hello["player"]
    rejected(worker, "observe", "unsupported_player", player=1 if player == 0 else 0)
    rejected(worker, "step", "invalid_request", player=player, ticks=3001, commands=[])
    rejected(worker, "step", "invalid_request", player=player, ticks=1,
             commands=[worker.stop(unit["handle"])] * 257)
    # The unit's current integer world point is a valid move destination.
    point = unit["position"]
    move = worker.move(unit["handle"], int(point["x_world"]), int(point["z_world"]))
    rejected(worker, "step", "invalid_unit", player=player, ticks=1,
             commands=[move, worker.stop("unknown-handle")])
    check(worker.observe()["observation"] == first["observation"],
          "rejected mixed batch changed observation")
    moved = worker.step([move], ticks=1)
    check(moved["ticks_advanced"] == 1, "move step failed to advance")
    stopped = worker.step([worker.stop(unit["handle"])], ticks=15)
    check(stopped["ticks_advanced"] == 15, "stop step failed to advance")
    return [first["observation"], moved["observation"], stopped["observation"]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    Path(args.scratch_root).mkdir(parents=True, exist_ok=True)
    framing_checks(args)
    worker = TrainingClient(args.native, args.game_dir, seed=123, timeout=args.timeout,
                            scratch_root=args.scratch_root)
    try:
        first = trajectory(worker)
        second = trajectory(worker, extra_observations=True)
        check(first == second, "same seed and commands produced different observations")
        print(json.dumps({"ok": True, "trajectory_ticks": [item["tick"] for item in first],
                          "stderr": str(worker.stderr_path)}))
    finally:
        worker.close()
    check(worker.process is None, "worker process remained open")
    with TrainingClient(args.native, args.game_dir, timeout=args.timeout,
                        scratch_root=args.scratch_root,
                        setup_args=["--training-limit", "2"]) as limited:
        end = limited.step(ticks=15)
        check(end["tick"] == 2 and end["ticks_advanced"] == 2 and end["truncated"],
              "episode limit did not truncate at exact boundary")
        rejected(limited, "step", "episode_ended", player=limited.hello_response["player"],
                 ticks=1, commands=[])

    for math_mode, pathfinder in (("hardware", "weighted"), ("off", "greedy")):
        with TrainingClient(args.native, args.game_dir, timeout=args.timeout,
                            scratch_root=args.scratch_root,
                            setup_args=["--experimental-math", math_mode,
                                        "--experimental-pathfinder", pathfinder]) as experiment:
            expected = {"math_mode": "hardware" if math_mode == "hardware" else "compatible",
                        "pathfinder": pathfinder}
            check(experiment.hello_response["simulation"] == expected,
                  "training experiment was not reported")
            first = experiment.step(ticks=15)["observation"]
            check(experiment.reset()["simulation"] == expected,
                  "reset changed the simulation policies")
            check(experiment.step(ticks=15)["observation"] == first,
                  "experimental training reset changed the seeded trajectory")


if __name__ == "__main__":
    main()
