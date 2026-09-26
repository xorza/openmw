# `film`: a demo video from a list of camera keys

Status: built. Section 7 is the implementation plan. Section 8 records what the first build adds
and leaves for tuning.

## 1. What it is for

A demo of the renderer: flights over the signature places, time-lapses from dawn to night, and a
sky that changes weather while the camera moves. The person flies a `view` window and presses
Home at each place worth a frame. The harness turns that list of keys into a PNG sequence and a
video.

- Two keys near each other are joined by a flight. The flight eases in at its first key and eases
  out at its last one.
- Two keys far apart are joined by a cut: a teleport, a warm-up the video does not show, then
  the next take.
- The hour and the weather of each key are part of the key. Between two keys, the clock runs
  forward from one hour to the other (a time-lapse), and the sky crosses from one weather to the
  other.

## 2. How it is used

```
rtx release view --keys=tour.keys        # fly; each Home press appends one key to tour.keys
rtx release film --keys=tour.keys --plan # print the takes, the segments and their lengths, and stop
rtx release film --keys=tour.keys        # render film/frames/000000.png..., then film/tour.mp4
```

The keys file is the block that Home already prints. A person can also paste the Home output of
several windows into one file. The comment lines are ignored.

```
[balmora]
cell = -3,-2
pos = -18075.145, -17586.46, 638.41016
look = -18625.098, -16765.438, 485.20374
hour = 6.5
weather = Overcast
day = 0
seconds = 12
hold = 3
cut = true
```

The last four fields are optional. `day` sets the moons. `seconds` is how long the flight to this
key takes, in place of the derived length. `hold` is how long the camera rests at this key. `cut`
set to true forces a cut before this key, and set to false forbids one. A comment is a whole line
that starts with `#`.

A key that leaves out `hour` stands at 12:00, and a key that leaves out `weather` stands under
`Clear`. That is the rule `describeBlock` already writes by, so the Home output reads back as the
same key. A section name can repeat: the file is a list, not a map.

## 3. What the field does, and what this takes from it

Sources are listed at the end.

- **Keys carry times, and a spline is timed by them** (ReplayMod's time keyframes, Unreal
  Sequencer's keys). A cubic Hermite spline through keys at given times gives a position that is
  continuous in velocity (C1). Catmull-Rom tangents, `m_k = (p_{k+1} − p_{k−1}) / (t_{k+1} −
  t_{k−1})`, are the standard choice for a camera that must pass through every key.
- **The time between two keys should follow the distance between them.** The ReplayMod forum
  traces most "weird camera paths" to keys whose spacing in time does not match their spacing in
  space. So a segment's length is derived from what changes over it, and a person gives a length
  only to override that.
- **A Catmull-Rom spline overshoots where neighbouring segments have very different speeds.** A
  flight followed by a pan on the spot makes the camera leave the spot and come back. The
  Fritsch–Carlson conditions remove the overshoot: a tangent is zero where the neighbouring
  secants change sign or one is zero, and the tangents of a segment are scaled into the circle
  of radius 3 (`τ = 3 / √(α² + β²)`). This is applied to each channel — x, y, z, yaw, pitch and
  hour. For the hour it also means that the clock never runs backwards.
- **Ease in and ease out at the ends of a move.** Every trailer camera eases in and out, and a
  cut from a moving camera looks like a mistake. The first and the last key of a take have a
  tangent of zero. With two keys, that is exactly smoothstep, `3u² − 2u³`. A `hold` rests the
  camera: zero tangents on both sides.
- **Pan speed rule**: the established limit for judder is one image width in seven seconds (RED,
  for 24 fps with a 180° shutter). The frames here have no motion blur, so the same limit is kept
  at 60 fps. A segment that turns the camera takes at least `|Δyaw| / hfov · 7 s` and
  `|Δpitch| / vfov · 7 s`. The horizontal field of view follows from `--fov`, which is vertical,
  and the aspect of `--size`.
- **Yaw and pitch, not quaternions.** The camera has no roll, so two angles describe it fully.
  Yaw is unwrapped against the previous key (the difference is taken into (−π, π]), so a turn
  goes the short way. Pitch stays well away from ±90° in any frame a person keys.
- **Encoding**: `ffmpeg -framerate <fps> -i %06d.png -c:v libx264 -preset slow -crf 18
  -pix_fmt yuv420p -movflags +faststart`. CRF 18 is the usual "visually lossless" point, yuv420p
  is what every player decodes, and faststart puts the index at the front for streaming. H.264
  needs even dimensions, so the filter pads an odd size by one pixel.

## 4. Timing a take

A take is a run of keys with no cut between them. Key `i` starts a new take where:

- it is the first key, or it says `cut = true`; or
- it does not say `cut = false`, and it is far from key `i − 1`: in another interior, inside
  where the other is outside, or more than `--cut-distance` units away (two exterior cells by
  default).

A segment from key `a` to key `b` takes `b.seconds` where the key gives it. Otherwise it takes the
longest of these:

| what changes      | seconds                                        | option             | default |
|-------------------|------------------------------------------------|--------------------|---------|
| the eye moves     | distance / speed                               | `--speed`          | 800     |
| the camera turns  | `max(|Δyaw| / hfov, |Δpitch| / vfov) · 7`      | `--pan-seconds`    | 7       |
| the clock runs    | forward hours · seconds per hour               | `--hour-seconds`   | 2       |
| the weather turns | a fixed crossing                               | `--crossing`       | 8       |
| nothing changes   | a still                                        | `--still`          | 4       |

A take of one key is a still of `max(hold, --still)` seconds. Every length is rounded to whole
frames at `--fps` (60 by default), so each key lands on a frame, and the video's frame `k` of a
take is its time `k / fps`, exactly.

Eight hundred units a second is about eleven metres a second: a drone flight, not a run.

## 5. What a frame stands under

The world is stepped at `1 / fps`, so the people, the water and the clouds move at their own
speed in the video, and two runs of one keys file are the same film.

- **The eye and the facing** come from the spline. The player's body is moved with the eye, with
  collision off, so the cells stream in around the camera and not around a body left behind on a
  hill.
- **The clock** is set to the spline's hour on every frame. The game's time scale is set to zero
  for the film, so the clock does not also run on its own. A hold at one hour stands still, and
  a time-lapse runs as fast as `--hour-seconds` says.
- **The weather** is held by the harness on every frame: the weather of the key before, the one of
  the key after, and how far the sky has crossed. The crossing uses smoothstep over the segment.
  The game has no way to do this: its transition runs on its own clock, at a rate each weather
  fixes, and a region change starts a transition of its own. So `WeatherManager` gets one call,
  `holdWeather`, which states the transition for the next update, and that update neither
  advances it nor replaces it. The call covers one update only, so no state stays behind.
- **A cut** is a new stop: a teleport, the history cleared, `--warmup` seconds drawn and not
  written, and every walk settled, so no cell arrives on screen.

## 6. How it fits the harness

Each take is one `Rtx::Stop`. The stop already has what a take needs: a place to teleport to, a
sky to start under, a warm-up, a count of measured frames and actions. A take adds two things:

- `Rtx::Schedule::mTrack` — a `Rtx::CameraTrack`, which is the keys at their frames with their
  tangents computed once, and which gives the pose at any frame of the take. The stop's route and
  weather turn are the bench's. The track is the film's.
- `Rtx::Actions::mFilm` — where each measured frame is written, and the number of the take's
  first frame in the video, so the file names do not depend on the order the frames come back in.

| where                                          | what                                                          |
|------------------------------------------------|---------------------------------------------------------------|
| `components/rtxbench/cameratrack.{hpp,cpp}`    | `CameraTrack`: keys, monotone Hermite tangents, the pose      |
| `components/rtxbench/benchrun.hpp`             | `Schedule::mTrack`, `Actions::mFilm`                          |
| `apps/rtxtool/film.{hpp,cpp}`                  | reads a keys file, splits it into takes, times the segments, prints the plan, makes the stops |
| `apps/rtxtool/run.{hpp,cpp}`                   | `describeKey`, what Home appends under `view --keys`          |
| `apps/rtxtool/session.{hpp,cpp}`               | follows a track, holds the clock and the sky, writes the frames |
| `apps/rtxtool/main.cpp`, `verbs.*`, `options.cpp` | the `film` verb, its options, the ffmpeg step              |
| `apps/openmw/mwworld/weather.*`, `worldimp.*`, `mwbase/world.hpp` | `holdWeather`                             |

## 7. Implementation plan

1. `WeatherManager::holdWeather` and `MWBase::World::holdWeather`.
2. `Rtx::CameraTrack`, with tests: smoothstep for two keys, the keys hit exactly, no overshoot
   beside a pan, yaw the short way, the hour never backwards, a hold rests the camera.
3. `film.{hpp,cpp}`: the keys parser, the take split, the segment lengths, the plan, and the stops.
   Tests: a pasted Home output reads back, repeated names, the rules for a cut, every length rule,
   and rounding to frames.
4. The session: follow the track on every frame (the warm-up at frame 0), hold the weather,
   advance the clock, collision off, time scale zero, and write the frames through a small map
   from the backend's frame number to the frame of the video.
5. `view --keys`: Home also appends `describeKey` to the file.
6. The verb: `film --keys`, `--out`, `--fps`, `--speed`, `--pan-seconds`, `--hour-seconds`,
   `--crossing`, `--still`, `--cut-distance`, `--plan`, `--encode`. The frames directory is
   cleared of the last film's frames first, because ffmpeg reads a sequence until the first gap.
7. Verify: the unit tests, then a real film of a few of `views.cfg`'s places at a small size.
   Look at frames from the start, the middle and the end of each take, and check the mp4 with
   ffprobe. Then the gate.

## 8. As built

Built as sections 4 to 7 say. What the first build adds, and what it leaves for tuning:

- **Offline, and the same film twice.** A film is stepped at `1 / --fps`, every walk is settled,
  and the validation layers are off unless `--validation` names them. Two runs of one keys file
  wrote the same 722 PNGs, bit for bit.
- **The day of a later key is not read.** A take starts on its first key's day, or `--day`, and the
  clock runs forward from there.
- **The per-channel limit can slow the camera at a bend.** Where a key is the turning point of one
  axis, that axis stops there for an instant. A curve through the key keeps its speed on the other
  axes, but the whole speed dips. A key's `seconds` is the fix for one that looks wrong.
- **No motion blur.** The frames are sharp, which is why the pan limit is the 24 fps one.
- **A stop's report names the sky of its last frame**, beside the hour of that frame. Before, it
  named the sky the stop started under, which a take that crosses into rain got wrong.
- **Home under `view --keys` was not pressed in a test.** The key it appends is `describeKey`,
  which the tests read back as a key.

Tuning knobs, all on the command line: `--speed`, `--pan-seconds`, `--hour-seconds`,
`--crossing`, `--still`, `--cut-distance`, `--warmup`, `--fps`, and per key `seconds`, `hold` and
`cut`.

## Sources

- [Catmull–Rom spline](https://en.wikipedia.org/wiki/Catmull%E2%80%93Rom_spline)
- [Monotone cubic interpolation (Fritsch–Carlson)](https://en.wikipedia.org/wiki/Monotone_cubic_interpolation)
- [ReplayMod: camera paths and time keyframes](https://www.replaymod.com/docs/)
- [ReplayMod forum: cubic spline "weird camera paths"](https://www.replaymod.com/forum/thread/2266)
- [Unreal Engine Sequencer keyframing](https://docs.unrealengine.com/4.27/en-US/AnimatingObjects/Sequencer/Overview/Keyframing)
- [RED: panning speed best practices](https://www.reddigitalcinema.com/red-101/camera-panning-speed)
- [Derek Lieu: a freecam for game trailers](https://www.derek-lieu.com/blog/2022/11/19/how-to-make-a-freecam-or-marketing-camera-for-game-trailers)
- [Arc-length parameterized spline curves (Wang, Kearney et al.)](https://homepage.divms.uiowa.edu/~kearney/pubs/CurvesAndSurfacesArcLength.pdf)
- [FFmpeg image sequences](https://en.wikibooks.org/wiki/FFMPEG_An_Intermediate_Guide/image_sequence)
