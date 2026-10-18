# Optional simulation experiments

The default simulation keeps compatible arithmetic and path searching. Two
optional settings allow performance experiments that can change game behavior:

| Option | Default | Experimental setting |
|---|---|---|
| `--experimental-math` | `off` | `hardware` |
| `--experimental-pathfinder` | `compatible` | `weighted` or `greedy` |

Enabling either experiment is allowed only for offline local AI benchmarks,
training workers, and headless runs with `--simulation-only`. Enabled
experiments are rejected for network play, recorded-game playback, saving and
loading. They are command-line settings for one process and are disabled on
the next ordinary launch. They do not change the format of game data or saved
records.

Hardware arithmetic changes only the pair rotation operation. It converts the
existing exact trigonometric table values to binary64, computes four products
and two sums with native binary64 arithmetic, and keeps each operation separate
with strict rounding and no multiplication/addition fusion. The compatible
path keeps the existing arithmetic and intermediate precision. Rounding can
change rotated coordinates and later simulation decisions. This option does
not replace every game-math operation with host math functions.

Weighted path searching raises the existing adaptive goal-estimate weight to
at least 1.75 when ranking search candidates. If its weight is already higher,
it keeps that weight. Greedy path searching instead ranks candidates by their
movement cost plus 24 times the raw goal estimate. Both keep movement costs,
map and footprint safety checks, and the set of accepted goals.

In six focused route cases, the conservative weighted mode reduced expansions
by approximately 0.7–22% while keeping all six route costs unchanged. Greedy
mode reduced expansions by approximately 10–27%, but one route cost increased
by 106%. These cases do not establish a bound on route quality in other maps.
Use weighted mode for the gentler experiment; greedy remains an explicit
research variant. The compatible mode remains the default. Either experiment
can change routes, arrival times, congestion, combat and match outcomes.

Measure each setting separately before combining them. For example:

```sh
python3 tools/benchmark_local_ai.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/simulation-experiments \
  --ticks 36000 --repeats 3 --seed 123 \
  --experimental-math hardware --experimental-pathfinder weighted \
  --validate-traces
```

The [local AI benchmark](local-ai-benchmark.md) records both settings in the
native configuration and requires repeated runs within a setting to agree on
their state and final digest. Optional trace validation compares complete
per-tick traces within that same setting. It does not require experimental
matches to reproduce compatible matches. The same seed and map can produce
different armies, tick counts and outcomes, so a speed ratio between modes can
also reflect a changed workload. Inspect workload windows alongside timing;
use focused arithmetic and search measurements to isolate their costs.

Explicitly restore both defaults with:

```sh
python3 tools/benchmark_local_ai.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/simulation-experiments \
  --experimental-math off --experimental-pathfinder compatible
```

The native regression check runs all six combinations twice and compares an
additional explicit-off run with the ordinary default run. Add full traces
when validating a change to these options:

```sh
python3 tools/check_native_simulation_experiments.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/simulation-checks --ticks 1800 --seed 123 \
  --validate-traces
```

Cross-mode differences are permitted; no short opening is required to differ.
The off switches must restore the default state and, when requested, the
complete trace. Correctness checks do not apply a performance threshold.
Run parser checks without an installed game using
`python3 tools/check_native_simulation_experiments.py --self-test`.
