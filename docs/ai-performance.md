# Measuring AI matches and recordings

See also [simulation and rendering measurements](simulation-performance.md).

Normal simulation runs at 30 ticks per second. Running ten times faster needs
at least 300 ticks per second. Five hundred times faster needs 15,000 ticks per
second. Measure a developed match as well as its opening:
the initial commanders give little indication of the cost of a growing army.

`--net-ai-benchmark TICKS` launches a headless network game with two opposing
hard computer players, Core and Arm, on Ashap Plateau when installed. Both human
slots watch. Each machine owns one computer player; both machines simulate the
match and exchange unit records over local networking. The run has no scripted
human orders, gifts, speed changes or forced defeats. Each computer player has
a 250-unit limit and 1,000 starting metal and energy. The
benchmark stops at the tick limit or when either computer has no live units.
This benchmark permits an AI-only game to continue while both humans watch;
ordinary multiplayer games keep their existing rule that ends an abandoned
game when no human participant remains. Commander rules and economy updates
retain their normal behavior. The commander rule is explicitly annihilation: a
player may continue after losing its commander, and is defeated when no units
remain. Each window also reports participation, controller presence, units
created and cumulative resource production so that a frozen economy or inactive
controller is visible.

The human-participant requirement is a runtime benchmark setting, outside saved
game state. This mode does not provide save/resume of an AI-only session.

The benchmark reports each 1,800-tick window's throughput, units and kills, then
its total ticks, elapsed time, speed multiple and peak unit count. Timing includes
both simulations, networking and periodic reporting. It excludes loading and
writing the final recording to disk. No frames are drawn in the timed loop.
This measures the cost of two networked simulations; a single training worker
has a different workload.

Run two independently recorded twenty-minute matches and verify their playback:

```sh
python3 tools/benchmark_ai_matches.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/ai-benchmarks \
  --ticks 36000 --repeats 2 --goal-speed 500 > local/ai-performance.json
```

The JSON contains the engine and recording hashes, full commands, process wall
time, native loop timing, window results and the requested speed target result.
Check the slowest windows as well as the aggregate. A growing battle can slow
down considerably after a quick opening. Logs and `.tad` recordings remain in a
new folder under `--scratch-root`. Keep those captures and game data out of Git.
Run benchmarks without concurrent builds or other simulation workloads.

Add `--compare-no-record` to pair every recorded match with a live match using
the same seed and tick limit without recording. Pair order alternates between
repeats. The report compares native loop costs, process wall costs and final
reported workload counters. Native loop timing includes collecting the recording;
writing the final file is outside the loop. Process wall timing includes that
write as well as loading and shutdown, so the wall ratio does not isolate file
serialization. No speed threshold makes a regression check fail.

Use an existing compatible recording for a repeatable replay workload:

```sh
python3 tools/benchmark_ai_matches.py \
  --native build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" \
  --scratch-root local/ai-benchmarks \
  --recording "/path/to/match.tad" --ticks 108000 --repeats 3
```

`--ticks` bounds the live match, or the length expected when replaying an existing
recording; playback allows another 1,000 ticks for trailing network records.
The tool requires a successful, fully consumed recording with compatible unit
definitions. Replays execute recorded network decisions and simulate remote
units; they do not run the original computer players' decision making. Use live
AI matches to assess AI costs and recordings to compare repeatable playback
workloads. The recorder's network updates can correct positions during playback,
so a clean replay alone does not prove simulation equivalence after an optimization.

The older `--net-loopback-each-ai` remains a network regression check: its computer
players are deliberately allied while scripted actions test network behavior.
It is a useful playback fixture, but is not opposing AI self-play.

For a direct native run, omit the Python runner:

```sh
SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy build/open-annihilation \
  --game-dir "/path/to/Total Annihilation" --skip-intro --mute \
  --net-ai-benchmark 36000 --seed 123 \
  --preferences-file local/ai.conf --net-record local/ai-match.tad
```
