#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Start a native episode, inspect owned units and issue a normal stop order."""
import argparse
import json
from training_client import TrainingClient


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root")
    parser.add_argument("--map", help="Optional installed map name")
    args = parser.parse_args()
    setup = ["--training-map", args.map] if args.map else []
    with TrainingClient(args.native, args.game_dir, scratch_root=args.scratch_root,
                        setup_args=setup) as worker:
        print(json.dumps(worker.hello_response, indent=2))
        observation = worker.observe()["observation"]
        units = observation["own_units"]
        commands = [worker.stop(units[0]["handle"])] if units else []
        result = worker.step(commands, ticks=15)
        print(json.dumps(result, indent=2))
        print(f"Worker diagnostics: {worker.stderr_path}")


if __name__ == "__main__":
    main()
