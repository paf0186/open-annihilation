# Python training worker client

The Python standard-library client starts the native game as a persistent
JSON-lines worker. Each reset starts a fresh process with fresh preferences.
Supply your installed Total Annihilation game data; no assets belong in Git.

```sh
python3 tools/training_example.py \
  --native ./build-check/open-annihilation \
  --game-dir /path/to/total-annihilation \
  --scratch-root /tmp/oa-training
```

Use `--map NAME` to select an installed map. The worker echoes its resolved
setup in the hello response. The example observes owned units, issues a normal
stop order and advances 15 simulation ticks. At 30 ticks per simulated second,
this advances half a second without wall-clock pacing or presentation drawing.

To use the client from a script in `tools/`:

```python
from training_client import TrainingClient

with TrainingClient("./build-check/open-annihilation", "/path/to/game") as game:
    observation = game.observe()["observation"]
    unit = observation["own_units"][0]
    result = game.step([game.move(unit["handle"], 800, 640)], ticks=15)
    result = game.step([game.stop(unit["handle"])], ticks=15)
    game.reset(seed=456)
```

To select an [offline simulation experiment](simulation-experiments.md), pass
its command-line settings through `setup_args`. They are retained across resets
and reported by the readiness handshake:

```python
with TrainingClient(
    "./build-check/open-annihilation", "/path/to/game",
    setup_args=["--experimental-math", "hardware",
                "--experimental-pathfinder", "weighted"],
) as game:
    print(game.hello_response["simulation"])
```

Move coordinates are whole world units: X/Z form the horizontal plane and Y
is elevation. They are neither screen pixels nor metres. Choose a destination
inside the resolved map dimensions. Handles are opaque, episode-scoped values;
obtain fresh handles after reset. An issued order does not guarantee arrival.

The first interface controls the configured local player against the built-in
opponent. Commands support move and stop. Observations contain owned units and
economy, and currently visible units. Radar contacts, remembered sightings,
arbitrary player views, attacks and production commands are not exposed.
Observe does not advance time; step accepts 1–3000 ticks and up to 256 commands.
The default episode limit is 108000 ticks; pass
`setup_args=["--training-limit", "3000"]` to change it. Inspect `terminated`,
`truncated`, `outcome` and `ticks_advanced` on each step.

`WorkerError` carries a valid native rejection in `.response`; validation
rejections preserve the worker and advance no time. `ProtocolError` means a
transport failure, timeout or invalid response, and closes the worker. The
client is synchronous and supports one caller. Its default response deadline
is 30 seconds, configurable with `timeout=`. Diagnostics remain in
`game.stderr_path` under a unique scratch directory. Close the client or use
the context manager to reap its process.

Run transport checks without assets:

```sh
python3 tools/check_training_client.py
```

Run the real engine check with an installed game:

```sh
python3 tools/check_native_training_worker.py \
  --native ./build-check/open-annihilation \
  --game-dir /path/to/total-annihilation \
  --scratch-root /tmp/oa-training-check
```

The native check covers framing, limits, rejected batches, local-player access,
move/stop, episode truncation, reset reproducibility and extra observations
between decisions. It compares policy observations; it does not claim complete
simulation-state equality or a training throughput measurement.
