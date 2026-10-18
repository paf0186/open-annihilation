# Optional arithmetic experiment

The default simulation keeps compatible arithmetic and path searching. One
optional setting allows a performance experiment that can change game behavior:

| Option | Default | Experimental setting |
|---|---|---|
| `--experimental-math` | `off` | `hardware` |

Enabling the experiment is allowed only for offline local AI benchmarks,
training workers, and headless runs with `--simulation-only`. It is rejected
for network play, recorded-game playback, saving and loading. It is a
command-line setting for one process and is disabled on the next ordinary
launch. It does not change the format of game data or saved records.

Hardware arithmetic changes only the pair rotation operation. It converts the
existing exact trigonometric table values to binary64, computes four products
and two sums with native binary64 arithmetic, and keeps each operation separate
with strict rounding and no multiplication/addition fusion. The compatible
path keeps the existing arithmetic and intermediate precision. Rounding can
change rotated coordinates and later simulation decisions. This option does
not replace every game-math operation with host math functions.

Measure the setting on the workload that matters. For example:

```sh
python3 tools/benchmark_local_ai.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/simulation-experiments \
  --ticks 36000 --repeats 3 --seed 123 \
  --experimental-math hardware --validate-traces
```

The [local AI benchmark](local-ai-benchmark.md) records `math_mode` and requires
repeated runs within a setting to agree on their state and final digest.
`pathfinder` remains `compatible`. Optional trace validation compares complete
per-tick traces within that same setting. It does not require experimental
matches to reproduce compatible matches. The same seed and map can produce
different armies, tick counts and outcomes, so a speed ratio between modes can
also reflect a changed workload. Inspect workload windows alongside timing;
use focused arithmetic measurements to isolate the cost.

Restore the default with `--experimental-math off`. The native regression check
runs both arithmetic settings twice and compares an additional explicit-off
run with the ordinary default run. Add full traces when validating a change:

```sh
python3 tools/check_native_simulation_experiments.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/simulation-checks --ticks 1800 --seed 123 \
  --validate-traces
```

Cross-mode differences are permitted; no short opening is required to differ.
The off switch must restore the default state and, when requested, the
complete trace. Correctness checks do not apply a performance threshold.
Run parser checks without an installed game using
`python3 tools/check_native_simulation_experiments.py --self-test`.
