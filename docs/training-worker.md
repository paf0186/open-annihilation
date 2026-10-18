# Training worker

The native game can run a two-player skirmish as a synchronous worker:

```sh
./run.sh --training-worker --game-dir "/path/to/Total Annihilation" --seed 123
```

`--training-map NAME` selects an installed skirmish map. Without it, the game
uses its first eligible map and reports the name. `--training-limit N` ends
an episode after N ticks (default 108000). A reset starts a fresh process;
there is no in-process reset. This first interface controls only the actual
local player against the existing computer player. It uses normal game
assets, orders and simulation ticks. It performs no per-tick drawing.

Standard input and output carry UTF-8 JSON objects, one per line. Diagnostics
use standard error. Send `hello` before observing or stepping:

```json
{"id":1,"op":"hello","version":1}
{"id":2,"op":"observe","player":0}
{"id":3,"op":"step","player":0,"ticks":15,"commands":[]}
{"id":4,"op":"close"}
```

Each response echoes `id` and contains `ok`. Hello reports version 1, the
local player index, map dimensions, seed, tick rate (30 ticks per simulated
second), episode limit and supported commands. Its `simulation` object names
`math_mode` and `pathfinder`, including explicit compatible defaults. Record
these settings with training results; [experimental modes](simulation-experiments.md)
can change behavior. Observe and step return
`tick`, `terminated`, `truncated`, `outcome` and `observation`. Step also
returns `tick_before`, `ticks_advanced` and command results. Commands are
issued before advancing complete ticks. Victory/defeat terminates an episode;
the tick limit truncates it. An engine fault leaves the worker faulted and
further stepping is refused. EOF or close ends the process.

An observation contains `player`, `tick`, `schema: "local-visible-v1"`, own
metal/energy and storage, `own_units`, `visible_units`, and
`radar_supported: false`. Units include an opaque process-scoped `handle`,
unit `type` and `position` with `x_world`, `y_world`, `z_world`. Own units
can receive move and stop orders:

```json
{"id":5,"op":"step","player":0,"ticks":15,"commands":[{"kind":"move","unit":"u1","point":{"x_world":800,"z_world":640},"queue":false}]}
{"id":6,"op":"step","player":0,"ticks":1,"commands":[{"kind":"stop","unit":"u1"}]}
```

Use a handle from the current observation. Handles are checked against slot
lifetimes and ownership, so stale handles cannot control replacement units.
Enemy handles retire when sight is lost. Move points are whole world units,
X/Z on the ground plane; Y is sampled from terrain. Coordinates must fall
inside the map and signed 16.16 range. Response positions retain fractional
world units. `queue` selects normal queued versus replacing move orders.
An issued order may subsequently fail during normal gameplay.

The worker bounds each line to 1 MiB, each batch to 256 commands and each
step to 1..3000 ticks. IDs must be integers in 0..9007199254740991. Unknown
or duplicate object fields are refused. Malformed requests and invalid
commands return `ok: false`, an `error.code`, and the unchanged tick. Whole
batches are preflighted before orders are issued. An unexpected engine
failure during issuance or simulation is `engine_fault`; it does not promise
rollback. Oversize framing exits nonzero. Error codes include
`invalid_request`, `unsupported_version`, `hello_required`,
`unsupported_operation`, `unsupported_player`, `invalid_unit`,
`invalid_command`, `out_of_bounds`, `episode_ended`, `engine_fault`, and
`setup_failed`. Startup failures may exit nonzero with stderr diagnostics
before a response is possible.

Observations omit radar/sonar contacts, remembered sightings, enemy economy,
health and order queues. They query the local player's displayed visibility;
they never refresh AI knowledge or draw simulation randomness. Other player
views are refused because existing contact state belongs to the configured
viewpoint. This interface is an initial control API, not a complete learning
environment or an arbitrary player observation API.
