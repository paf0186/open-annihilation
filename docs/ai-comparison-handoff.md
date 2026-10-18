# AI comparison handoff

Research snapshot: 2026-10-18. Continue on the
`training/simulation-runner` branch of
[paf0186/open-annihilation](https://github.com/paf0186/open-annihilation).
This is a research and implementation handoff, not completed AI evaluation.
Keep the existing map and game assets fixed; new mods and maps are out of scope.
Each machine needs its owner's installed Total Annihilation 3.1c data.
Do not commit game assets, extracted profile text, captures or generated logs.

## Available options

| Option | Current status | Next work |
|---|---|---|
| Stock hard Arm/Core | Working local benchmark baseline | Establish evaluation seeds and outcomes |
| Stock profile variants | Installed profiles configure the same controller | Add explicit selection while holding the map fixed |
| Native scripted/hybrid controller | Recommended first different controller; not implemented | Economy, construction, production and attack decisions |
| Compact learned policy | Python transport exists; no trained policy | Broaden actions, measure the complete loop, then train |
| Spring bots | Reference code for a different engine API | A substantial adapter/port and license review |

The inspected installation contains DEFAULT, AirBattle, SeaBattle, Hover,
Metal, Waterwrld, Acid, Urban and MISSIONS profiles in rev31.gp3; ccdata.ccx
also contains Krogoth. Availability can differ between installations.
The runtime selects profiles through session/map metadata, with DEFAULT as
fallback; there is no explicit profile CLI selector yet. See
`Runtime::session_ai_profile_path` and `read_computer_profile` in
`src/app/runtime_campaign.cpp`. Local benchmarking currently fixes hard
Arm/Core opponents. Profile names do not establish suitability or strength
on the fixed map. No alternative profile/controller comparison has run.

[Spring's AI directory](https://github.com/spring/spring/tree/develop/AI/Skirmish)
and [AAI](https://github.com/spring/AAI),
[KAIK](https://github.com/spring/KAIK),
[Shard](https://github.com/spring/Shard) and
[HughAI](https://github.com/spring/HughAI) provide architecture references.
Their callbacks, unit tables and game configuration target Spring, not this
engine. They are not drop-in TA bots. Verify exact licenses before copying
code; the research did not establish complete reuse terms for every project.

## Existing entry points

- [Local AI benchmark](local-ai-benchmark.md): one simulation world, two stock
  controllers, workload counters, normalized digests and trace validation.
  Use `tools/benchmark_local_ai.py` for repetitions; its speed target is a
  performance metric, not a playing-strength score.
- [Python client](training-python.md) and [worker protocol](training-worker.md):
  synchronous observe/step against the stock opponent; reset starts a fresh
  process. Currently only move/stop actions are exposed. Attack, construction
  and production are missing, so this is not yet a full-game learning API.
- [Network AI benchmark](ai-performance.md): playable `.tad` capture/replay.
  Local benchmark traces are validation artifacts, not playable recordings.
  Replay omits the original controller's decision cost.

## Smallest useful next comparison

1. Add an explicit stock profile selector, validate names against installed
   data, and report the resolved profile and difficulty per player. Keep
   DEFAULT as the unchanged default; expose the selector to the benchmark.
2. Compare DEFAULT with one installed alternative on the same map, rules,
   resource settings, difficulty and unit cap. Use paired seeds and swap
   sides/factions. First verify each run has active controllers, production
   and developed armies; do not count inactive tails as useful throughput.
3. Report wins, losses, draws and tick-limit truncations separately, plus
   game duration and economy/army counters. Predetermine the seed set and
   stopping rule; retain raw per-match results, engine revision, binary hash,
   compiler flags and local content hashes. Do not equate a faster match with
   a stronger controller. Existing repeated-state checks measure determinism,
   not strength; mixed-profile matchup/outcome reporting still needs wiring.
4. Measure simulation, observation/serialization, transport and policy CPU
   time separately and together. Run independent match processes; keep timed
   trials free of competing builds/tests. Preserve ordinary visibility rules
   for external policies and state any privileged information explicitly.

For a genuinely different opponent, implement a small native scripted baseline
behind a narrow observation/action interface before attempting a Spring port.
Expose attack, construction and production with legal-action validation to
both native and Python policies. Keep controller selection explicit and stock
behavior available; commit independently reviewable pieces.

Only then consider [Gymnasium](https://gymnasium.farama.org/) with
[Stable-Baselines3 PPO](https://stable-baselines3.readthedocs.io/en/master/modules/ppo.html),
optionally [recurrent PPO](https://sb3-contrib.readthedocs.io/en/master/modules/ppo_recurrent.html).
A compact policy on one 12 GiB GPU with CPU inference is a plausible target,
not a demonstrated result. Native headless speed measurements exclude policy
inference and Python transport. Measure before changing language or choosing
model size. No dependencies, bots, mods or maps were installed for this handoff.
