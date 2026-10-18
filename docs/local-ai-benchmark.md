# Measuring local AI matches

Normal game speed is 30 simulation ticks per second. Five hundred times that
speed requires 15,000 ticks per second. Measure a developed battle as well as
the opening: two commanders cost much less to simulate than growing armies.

`--local-ai-benchmark TICKS` starts two opposing hard computer players, Arm and
Core, inside one local skirmish. Both players have their normal AI controllers
from match initialization. Each has a 250-unit limit and 1,000 starting metal
and energy, with annihilation rules. The map defaults to Ashap Plateau when
installed; use `--local-ai-map NAME` to select another installed map. The run
ends at its tick limit or a natural match ending. It does not draw, exchange
network messages, wait for real time, or inject scripted orders.

The benchmark uses the ordinary skirmish bootstrap and match simulation. It
keeps the normal unit pool, including slots reserved for other player seats.
Reported configuration includes the selected map, seed, active players, pool
size and rules. Each 1,800-tick window reports actual advanced ticks, elapsed time,
units, kills, participation, controller presence, units created, and cumulative
metal and energy production. These counters expose inactive controllers or a
frozen economy instead of counting an idle tail as fast play. The final report
includes a normalized world digest.

Run three independently initialized twenty-minute matches:

```sh
python3 tools/benchmark_local_ai.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/ai-benchmarks \
  --ticks 36000 --repeats 3 --seed 123 --goal-speed 500 \
  --validate-traces > local/local-ai-performance.json
```

Add `--map "Ashap Plateau"` to require a particular map. The JSON retains the
binary's SHA-256, configuration, complete commands, logs and every workload
window. It reports the median repetition speed, aggregate throughput and the
slowest window. Developed-window statistics use windows that start after
18,000 ticks by default; `--warmup-ticks` changes this boundary. A short run can
have no developed windows. Inspect their counts and army sizes before drawing
conclusions about sustained play.

Native loop timing includes periodic reporting and excludes loading. Process
wall time also includes startup, loading and shutdown. Optional full per-tick
trace checks run in two additional processes, outside the timed repetitions.
The tool validates their record counts and ordering, compares complete trace
files byte for byte, and requires the same reported state as the timed run.
Repeated runs must agree on configuration, all reported workload counters and
the final digest. A failed correctness check fails the command; missing the
requested speed target is reported without failing it.

Optional [simulation experiments](simulation-experiments.md) select hardware
rotation arithmetic. The runner accepts the same `--experimental-math off|hardware`
option as the game. The default keeps compatible behavior. Native configuration
records `math_mode` and `pathfinder` (always `compatible`), and repeated-state
comparisons include them. Experimental modes
must repeat themselves; their state need not match another mode.
Older binaries that omit these configuration fields are accepted only when
compatible modes are requested; their binary hashes identify the older build.

For a shorter native regression check:

```sh
python3 tools/check_native_local_ai.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/ai-checks \
  --ticks 1800 --repeats 2 --seed 123 --validate-traces
```

This check also requires observed unit creation or resource production. Parser
checks need no game installation:

```sh
python3 tools/benchmark_local_ai.py --self-test
```

Keep logs, traces and game data outside Git. Run measurements without concurrent
builds or other simulation workloads. The `OA_TEST_RUNNER` environment variable
can name a command that launches a cross-built executable.

The [network AI benchmark](ai-performance.md) remains useful for capturing and
replaying `.tad` recordings. It runs two complete networked match worlds; this
benchmark runs one world with both local AI controllers. Their ownership and
random-stream configurations differ, so a local run is a separate workload.
Do not infer a single-world speed by doubling the network benchmark's result,
or expect their world digests to match. Local AI state reports and traces are
validation captures; they are not compatible game recordings. Replaying a
network recording executes recorded decisions and omits the original AI's
thinking cost.
