# Director scripts

Two options of the game, `open-annihilation`, turn a recorded game into a
video: `--generate-script RECORDING` watches the recording from start to end
and plans where the camera looks, writing a director script, and
`--render-script SCRIPT` replays the recording and draws the script's shots
at its size and frame rate, with the game's sound heard from the camera,
into MP4 files. A script can also be written or edited by hand, and can
name a stage in place of a recording: a file of units placed and orders
given at set ticks, which the game plays out over a skirmish
([Rendering a stage](#rendering-a-stage)).

Both need two things besides the game:

- **Network play, which replays the recording.** The game hands the
  recording's bytes to the extensions of the build (`Extension::open_recording`,
  [src/app/README.md](../src/app/README.md#extensions)), and the one that
  recognises them starts the recorded game and steps it one tick at a time.
  Network play ([src/app/README.md](../src/app/README.md#network-play)),
  always built, replays 3.1c's recorded games (`.tad`). A recording no
  extension of the build replays stops the run with
  `no extension of this build replays NAME`.
- **The `ffmpeg` program on `PATH`**, built with `libx264`, for the videos
  (as for [capture.md](capture.md)). `OA_DIRECTOR_ENCODER=none` renders
  without it: every frame is drawn and hashed and the sound and manifests
  are written, but no video is made.

Both runs are headless: no window opens, and SDL's video and audio are never
started. Both run on the fixed clock and the fixed random seed of the
headless checks (`--headless-check`), so that a script's analysis and its
render replay the recording alike.

## Generating a script

```sh
open-annihilation --generate-script games/game.tad
open-annihilation --generate-script games/game.tad --output videos/game.oascript --resolution 3840x2160
open-annihilation --generate-script games/game.tad --output videos/game.oamovie
```

The recording is replayed to its end without drawing, in the exact order a
render replays it; a timeline of the game is kept as it runs (every unit
created, finished and killed, every shot, detonation and hit, the positions
of the moving units twice a second and the players' totals once a second),
and the shots are planned from the whole timeline, so the camera can arrive
before something happens. The plan follows the first commander from a view
of the whole map; in the quiet parts of the game it takes each player's
side in turn, two shots at a time, showing a commander building, a
factory's first units, a construction unit expanding or a base being built,
at varied heights and lengths and drifting or pushing in slowly, with now
and then a wide drift from one side to the other. It follows armed groups
advancing on an enemy base, cuts to where most damage lands in the next
seconds and stays with a battle while it goes on, frames long-range fire
wide from launch to impact or shows a gun firing and then its shell
landing, interrupts for commander deaths and large blasts, and ends on the
commander death that decides the game under the commander-death victory
rule, held and pulled back: the first after which at most one side has a
commander. An annihilation or deathmatch game, or one no commander death
decides, is directed to its end; a commander the replay replaces in its
slot with another of its player's on the same tick is no loss. A subject
whose view overlaps one shown in the last minute counts for less, so the
camera does not go back and forth between the same places; views are
placed to show land rather than water, and the camera moves only when its
subject nears the edge of the view. Near subjects are panned to; far cuts dissolve, a cut to the other side of the
map wipes now and then, a big moment comes in with a checkerboard, and the
first battle and the ending fade through black. The rules and their
constants are in `src/media/include/oa/media/director/planner.hpp`. The run
prints one line for each change of subject.

The replay must be clean for the script to cover it: a replay that does
not finish is refused; one that fails part way (a record that cannot be
applied, a tick that fails) is cut at the failing tick, with a warning,
unless that is less than a minute in. The recording must be one this
installation replays exactly: its unit definitions must be the
installation's, its map installed and a slot free to watch it from.

`--output` names the script (`.oascript`) or a bundle (`.oamovie`, below);
by default the script goes beside the recording, `RECORDING-STEM.oascript`.
A script names a recording in its folder or below it by its path from that
folder, and any other recording by its absolute path.
`--resolution WxH` sets the frame size the script plans for (even, 16 to
16384 pixels a side); the default is 1920x1080. The frame rate is 60 frames
a second, the game's 30 ticks a second, cut into one-minute chunks. The same
recording always gives the same script, byte for byte, on every platform.

## Rendering a script

```sh
open-annihilation --render-script videos/game.oascript
open-annihilation --render-script videos/game.oamovie --output /tmp/game-video --chunks 3-5
OA_DIRECTOR_PRESET=veryfast open-annihilation --render-script videos/game.oascript
```

The video's frames are cut into chunks (the script's `output.chunking`),
each written as its own files, so that a long game renders and encodes in
parts and a part can be rendered again alone. In the output directory, by
default the script's folder and stem (`videos/game/` for
`videos/game.oascript`), a script named NAME makes:

| File | What it holds |
|---|---|
| `NAME-000.mp4`, ... | each chunk's video and sound: H.264 (libx264, High profile, 4:2:0, BT.709, constant quality 18, closed groups of half a second of frames) with AAC at 48 kHz and 384 kbit/s, the index at the file's start |
| `NAME-000.wav`, ... | each chunk's sound: 16-bit stereo PCM at 48 kHz, exactly the chunk's samples |
| `NAME-000.frames`, ... | each chunk's frames, one line a frame: `<frame> <tick> <sha256>`, the frame's RGB bytes hashed |
| `NAME.mp4` | every chunk joined: the chunks' frames as they are and the joined sound encoded once |
| `NAME.manifest` | the engine version, the script's and recording's SHA-256, the size and rates, the ticks and frames, and each chunk's frame range, frame-manifest SHA-256 and sound SHA-256, then the whole sound's |
| `NAME-still-000120.png`, ... | each frame `--stills` names, as drawn: an 8-bit RGB PNG of the frame's size, without loss; the frame's number has at least six digits |

Chunk numbers have at least three digits. The joined video and the run
manifest are made only when every chunk was rendered. While they are
needed, each chunk's encoded frames (`NAME-000.video.mp4`), the joined sound
(`NAME.wav`) and the list of chunks to join (`NAME.concat.txt`) are kept too;
they are removed once the render is done, and a render that stops on an
error leaves them.

`--stills F,F...` also writes the frames it names, counted from 0 and in
increasing order, as pictures; each must lie in the video and in a chunk
the render draws, and the run prints a line for each with the tick it
shows. Frame *f* of the video is *f* frames after the first shot's tick
began, so at the default 60 frames a second frame 2 × *t* shows tick
*first* + *t*.
`--chunks A-B` (or `--chunks A`) draws and encodes chunks A to B alone,
counted from 0. The ticks before them are still replayed, and their sound
still mixed, so that a chunk rendered alone sounds as it does in a whole
render; the ticks after them are not replayed. `--mute` renders silence.
`--output DIR` names the output directory. `OA_DIRECTOR_PRESET` names
libx264's preset (`medium` by default; `veryfast` encodes about three times
faster into larger files).

A script without `endTick` ends with its recording. When the extension does
not know where the recording ends, the render first replays it to its end
without drawing, then again to render it.

The run prints a line for each chunk and one at the end with the replay's
state; errors of the replay while rendering are warnings. It exits with 0
when every chunk asked for was written, and with 1, after one line
`open-annihilation: director: ...` on standard error, when anything failed.

Frames are drawn on one thread, hashed on up to eight others and encoded
by `ffmpeg` on the rest of the machine. On the 24-core desktop this was
measured on, a one-minute chunk from the middle of a two-player game at 4K
and 60 frames a second took about 60 s to draw and hash and 95 s with the
default preset's encoding; 30 s of the game's start at 1280x720 took 4 s,
and 24 s with encoding. `OA_DIRECTOR_ENCODER=none` shows the drawing alone.

## Bundles

A bundle (`.oamovie`) is a zip archive holding one script at its root,
`NAME.oascript`, and the recording its `input.demo` key names, by entry
name. `--generate-script ... --output NAME.oamovie` writes one, storing its
entries uncompressed with fixed times, so the same recording always gives
the same bundle; `--render-script` reads it in memory, deflated entries
included. A bundle is at most 1 GiB, and its script at most 16 MiB.

## The script

A director script is a text in YAML (a subset of YAML 1.2) or in JSON; the
first character that is not white space, `{`, chooses JSON. Both read the
same way. The YAML subset holds block and flow mappings and sequences,
plain and quoted scalars and comments; anchors, aliases, tags, block
scalars, directives, several documents, duplicate keys, tabs in
indentation, hexadecimal and octal numbers, `.inf` and `.nan` are refused,
each by name with its line and column. Numbers are exact decimals: `29.97`
is 2997 hundredths, never a binary approximation. Every error and warning
names its key, such as `director.shots[2].cameraEnd.position.y`.

```yaml
oascript: 1
input:
  demo: game.tad
  tickrate: 30
output:
  resolution: { width: 1920, height: 1080 }
  framerate: 60
  chunking: { mode: seconds, length: 60 }
  showUx: false
director:
  shots:
    - tick: 0
      cameraStart: { position: { x: 1664, y: 1872, z: 944 } }
      cameraEnd: { position: { x: 700, y: 540, z: 1500 } }
      motion: { spring: { frequency: 0.25, dampingRatio: 1 } }
    - tick: 600
      cameraEnd: { position: { x: 900, y: 600, z: 1400 } }
    - tick: 1200
      cameraStart: { position: { x: 2600, y: 540, z: 400 } }
      cameraEnd: { position: { x: 2500, y: 700, z: 450 } }
      transition: { type: dissolve, duration: 0.5 }
  endTick: 3600
```

| Key | Meaning | Default and limits |
|---|---|---|
| `oascript` | the format's version | required; 1 |
| `input.demo` | the recording: its absolute path, its path relative to the script's folder, or its entry name in a bundle | required unless `input.stage` is given |
| `input.stage` | a stage file to play in place of a recording: its absolute path or its path relative to the script's folder ([Rendering a stage](#rendering-a-stage)) | none; a script names a demo or a stage, not both, and a bundle carries a recording only |
| `input.tickrate` | game ticks a second of video: 30 is the game's normal speed | 30; above 0, at most 3000, at most 3 decimal places |
| `output.resolution` | `width` and `height` of the frames, pixels | 1920 by 1080; even, 16 to 16384 |
| `output.framerate` | frames a second | 60; above 0, at most 240, at most 3 decimal places |
| `output.chunking` | `mode: seconds` cuts every `length` seconds of video, `mode: ticks` every `length` game ticks (a whole number) | seconds, 60 (ticks: 1800); at most a day |
| `output.showUx` | draw the game's interface around the battlefield (see limits) | false |
| `director.shots` | the shots, in increasing tick order | required; 1 to 100000 |
| `director.endTick` | the first tick the video does not show | the recording's end; after the last shot's tick |

Each shot:

| Key | Meaning |
|---|---|
| `tick` | the game tick it starts on (required) |
| `cameraStart` | the camera at its start; left out, the shot continues from where the shot before ends, its motion's speed included (the first shot needs one) |
| `cameraEnd` | the camera at its end (required) |
| `motion` | `{ linear: {} }` (the default): constant speed; or `{ spring: { frequency: HZ, dampingRatio: D } }`: a damped spring toward the end, `frequency` above 0 and at most 20, `dampingRatio` above 0 and at most 10 (1 by default, which settles without overshoot) |
| `transition` | how its first frames take over from the shot before: `type` `fade` (through black), `wipe` (in from the left), `dissolve` or `checkerboard`, over `duration` seconds (above 0, at most 60, at most 3 decimal places); the first shot has none |

A camera is `{ position: { x: X, y: Y, z: Z } }`: `x` and `z` are the centre
of the view on the ground, in map pixels (`z` is the row of the map's image:
a point's height is already folded in), and `y` is the height of the map the
view shows, in map pixels, so a larger `y` shows more of the map. The game's
view cannot turn: an `orientation: { pitch, yaw, roll }` is read, ignored,
and a non-zero angle is a warning. A view is kept on the map: its height
between 270 map pixels and the height at which the whole map fits the frame,
its centre so that the view shows nothing past the map's edges; a camera
that has to be moved for that is a warning. The zoom (frame height over view
height) is not limited to the player's range. `linear` moves `x`, `z` and
`1 / y` evenly, so the zoom changes at an even rate.

A shot runs from the first frame that shows its tick to the first frame of
the next shot; frame *f* shows the game after tick `first shot tick + floor(f
* tickrate / framerate)`. A frame whose time, `first shot tick + f *
tickrate / framerate`, lies between two ticks is drawn from the later tick,
with units (their place, heading and the pieces their scripts move and
turn), projectiles, debris, fragments and particles shown that part of the
way from the earlier tick, as the game draws frames between ticks, whatever
[Smooth motion](settings.md#graphics) says; a frame on a whole tick is the
tick as it is. With the defaults every other frame
lies half way between two ticks, so units move 60 times a second, as the
camera does. The last frames, which would need the tick no frame shows,
are drawn from the tick they show. Game time is never skipped. A
transition's frames draw the tick twice, from the shot's own camera and
from the view the shot before ended on, held still, and blend them.

Scripts are read and written by `src/formats/oascript` (the grammar and
every limit are in its headers), and compiled into cameras by
`src/media` (`oa/media/director.hpp`, `clock.hpp`, `transition.hpp`).

## Rendering a stage

A script whose `input.stage` names a stage file renders a fight set up for
the video instead of a recorded game. The run starts the two-player
skirmish the headless checks start, on the map, sides and starting
resources the run's preferences file gives (`--preferences-file`), carries
out the stage's lines as `--stage` does, and plays the match on the fixed
clock and seed, the stage's lines timed for later ticks running as their
ticks come. Its actions ([src/app/README.md](../src/app/README.md)) place
units by map pixel or from a player's first unit, gather them into groups
and give the groups orders; for example:

```text
# Three of the local player's units hold a line; the other player's
# arrive in two waves, ten seconds apart, and attack the nearest of them.
group line
place 0 ARMPW 2400 2000 north
place 0 ARMPW 2480 2000 north
place 0 ARMPW 2560 2000 north
group wave1
place 1 CORAK 2400 1200
place 1 CORAK 2480 1200
attack wave1 line
at 300 group wave2
at 300 place 1 CORTHUD 2440 1100
at 300 attack wave2 line
```

A stage has no end of its own, so its script must set `director.endTick`,
and the render keeps on to that tick whatever happens; `stage:` lines
report what the stage did as it goes. A stage plays out alike on every run
and platform, drawn or not, as a recording does. The other player is a
computer player, whose own decisions reach its units beside the stage's
orders, unless a `console` line of the stage hands it to no one: the
console's `+AI 1` does where the game's rules let that command be typed
in a skirmish.

## Sound

The sound is the game's sound effects as heard from the director's camera:
each tick is replayed with the camera of the first frame that shows it
whole, which places the tick's sounds, and a sound starts on the first
sample of that frame, though a frame before it is drawn part of the way to
the tick. Every sound the game plays at a point is placed in stereo from
the view's centre, as the game places sounds with 3D sound on, and every
player's units speak (their acknowledgements, build and under-attack
announcements), placed at the unit; the game itself speaks only for the
viewing player. The mix (`src/audio`, `oa/audio/offline_mix.hpp`) keeps the
game's voice policy, eight voices at once with the oldest giving way, at
the game's default effects volume, and runs on the frame clock, not on a
sound device. There is no music.

## Determinism

A script and its render are the same on every platform and every run:

- the replay runs on the fixed clock and seed; `--seed` cannot be used;
- the camera never changes the game: drawing changes nothing the game
  reads. The game places the pieces it aims at from the units themselves,
  never from the transforms a draw rebuilt, and the debris of explosions
  starts its particles in the game's own tick. What the game does does not
  depend on which frames were drawn, from where, or whether any was: the
  generator's undrawn replay and every render reach the same world;
- the script's numbers, the frame clock, the chunks, the transitions, the
  mix and the planner use exact integer arithmetic; the camera uses IEEE
  doubles with addition, subtraction, multiplication, division, square root
  and floor alone, in a fixed order; the renderer draws on the CPU;
- the frame manifests and the sound's SHA-256 in the run manifest are the
  check. MP4 files are not expected to match across `ffmpeg` versions.

`native-director-render` renders a small script over the headless skirmish
with encoding off, whole and its second chunk alone, and pins the frames'
and the sound's hashes; `native-stage-render` renders a script that names a
stage, twice, with two stills, and checks that both renders draw the same
frames; `native-director-render-relative` renders it with
its files and preferences named relative to the working directory and pins
the same hashes; `native-director-view` checks the frames director mode
draws; `app-director-output` checks the files, the encoder's arguments and
bundles.

## Limits

- **Network play must be built.** Without it, or another extension that
  replays the recording, neither option does anything but say so.
- **`showUx`** draws the interface the game draws around a battlefield of
  the frame's size, from the camera's whole map pixel; the interface keeps
  timers of its own, so frames with it may differ from run to run.
- **Chunks rendered alone** sound as they do in a whole render, and the game
  plays out alike, but their first frames may not match a whole render's
  pixel for pixel: models' texture animations and cached images start with
  the first frame drawn.
- **What changes at the tick rate alone.** Between ticks only places,
  headings, piece poses and spins are shown part of the way. A piece that
  is spinning follows that spin, including a turn of more than half a
  revolution, so it never comes back the other way between ticks. Sprite
  and texture animation frames, smoke puffs, the nanoframe's pulse and
  units that appear, jump or are loaded and dropped change at the tick.
- **A map smaller than the smallest view** (270 map pixels high, or 480
  wide at 16:9) is drawn from its top-left corner.
- **Drawing is slow when many units show.** The renderer draws on the CPU,
  on one thread: a frame costs a few milliseconds early in a game and tens
  of milliseconds late in it at a view of the whole map, at any size;
  transition frames are drawn twice.
- **Unit speech** comes from what the game asks its units to say; units of
  a recorded game are not given orders, so they say little: in one
  23-minute two-player game, 127 announcements against 968 other sounds.
- **No music**, and no sound the game plays for its interface.

## Code

`src/app/runtime_director.cpp` runs both options and the render check, and
starts a stage's skirmish (`src/app/runtime_stage.cpp` plays its lines);
`src/app/runtime_director_view.cpp` is director mode (the camera, the
drawing and the sound routes, `director_presentation.hpp`);
`src/app/director_output.cpp` writes the files and runs `ffmpeg`
(`director_output.hpp`); `src/media` plans shots from a timeline and steps
the cameras; `src/audio` mixes; `src/formats/zip` reads and writes bundles.
