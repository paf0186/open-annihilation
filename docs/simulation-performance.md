# Measuring simulation and rendering

Normal play advances 30 simulation ticks per second. A 500 times speed target
therefore needs 15,000 ticks per wall-clock second. Report the workload and its
unit counts along with throughput: an opening with two commanders does not
represent a developed battle.

`--headless-check --simulation-only --match-ticks N` advances the complete match
without drawing every tick. Its native loop timer excludes loading, the final
world digest and the optional final snapshot. Periodic logging remains inside
the timer. Process wall time includes those other costs. Use fresh preferences,
a fixed seed and the same installed data when comparing executables.

The profiling helper supplies repeated normal movement orders to four squads,
an idle local player against a developing computer opponent, or staged combat:

```sh
python3 tools/profile_simulation.py \
  --native build/open-annihilation --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/profiles --case movement --ticks 6000 \
  --squad-units 32 --repeats 3 > local/movement.json
```

Movement is a pathfinding stress case on Canal Crossing, using fresh default
preferences. Activity windows and the number of live units receiving orders
show when losses change the workload. Longer runs can become dominated by the
surviving opponent after the scripted squads die. `--gprofng` collects a separate
profile; verify the collector's actual sampling interval before interpreting
sample weights. Profiled throughput includes instrumentation overhead.

For genuine opposing computer players and recorded-game playback, use the
[AI match helper](ai-performance.md). For Python-controlled episodes, use the
[training client](training-python.md).

## Rendering enabled

The rendering helper compares the same seeded battle without per-tick drawing,
with a frame drawn after every tick, and with two frames per tick. This example
uses 1920 by 1080 pixels and the highest anti-aliasing setting in every case:

```sh
python3 tools/benchmark_rendering.py \
  --native build/open-annihilation --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/rendering --suite max-quality \
  --ticks 60 --combat-units 16 --repeats 3 --validate-traces \
  > local/rendering.json
```

Headless rendering composes the actual battlefield image on the CPU. The
helper retains snapshots, native loop times, process wall times and final
world digests. `--validate-traces` checks the complete per-tick digest streams
in separate passes, so trace output does not affect the timed runs.

For a human display cadence while simulation advances without waiting, use
`--suite wall-clock --ticks 6000` instead. It compares undrawn simulation with
the same battle drawn at most 30 or 60 times per wall-clock second, using
`--max-fps 30` and `--max-fps 60`. This suite defaults to anti-aliasing 4;
`--wall-clock-aa 16` requests the maximum setting. The first frame is drawn immediately; missed
display deadlines are skipped. After drawing, at least one display interval is
available for simulation before the next frame. A slow renderer can draw fewer
frames than the limit. Actual frame counts and rates are reported. `--max-fps 0` or omitting
the option retains drawing every tick in this headless runner.

`--presentation` additionally exercises SDL upload and presentation through its
offscreen driver, without opening a visible window. This requires a working
offscreen graphics backend. On some Linux installations its OpenGL library must
be named with `--opengl-library`. `--force-presentation` also tries Full rendering
with the benchmark-only capability override. Reports retain the requested and
actual tier, software-renderer diagnostics and runtime fallback events. A
software graphics driver does not measure physical GPU performance.

Presentation uses the engine's four existing skirmish and campaign benchmark
scenes. It is reported separately from the active battle comparison. No scene's
presentation rate alone establishes accelerated simulation throughput.

`--frame-rate 60` means 60 frames per simulated second. At 500 times speed this
would require 30,000 drawn frames per wall-clock second. Displaying 60 frames per
wall-clock second while advancing 500 times faster would instead require many
simulation ticks between displayed frames. These are different rendering costs.

Run timing comparisons without concurrent builds or other game instances.
Compare repeated runs, retain their spread, and check identical per-tick state
traces against the previous executable before attributing speed changes to an
optimization. Keep installed data, recordings, captures and profile files out
of Git.

## Accelerated offline play

Launch `./run.sh --offline-speed 1000 --max-fps 60` to request 1000 times
normal simulation speed in a live offline skirmish or campaign. Targets from
1 through 10000 are accepted; actual speed depends on the battle and processor.
The window title shows target and achieved speed, and `+stats` shows ticks per
wall-clock second. Plus doubles the target, minus halves it down to 1x, and
Pause pauses. The game menu holds the simulation without accumulating work.

Simulation still advances every ordinary tick. The application returns to
input and drawing after at most 12 ms of simulation work, except that a single
slow tick must finish. Drawing stays at most 60 wall-clock frames per second,
or the lower configured cap. Large wall-clock delays do not accumulate an
unbounded backlog. Computer-player timers use simulation time, and timed map
units are checked after every accelerated tick.

This option controls pacing separately from the saved game's ordinary speed.
The GAME speed slider is locked while it is active; an existing game-speed
slider in SHARE, when a profile supplies one, selects powers of two instead.
Resource-sharing sliders retain their normal meaning. The target is not saved
or transmitted. Network games, replay playback, movie capture and fixed-clock
run modes cannot use this option. Omit it to retain the ordinary speed controls.

`--check-offline-speed` runs a bounded rendered check of one-tick versus batched
trajectories, wall-clock acceleration beyond five ticks per frame, Pause input,
speed controls and unchanged canonical speed fields.
