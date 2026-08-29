# Judging a moving frame

Every defect that mattered this session — terrain that blinks, an NPC's limbs left behind — lives
only in motion, and the tree has no way to judge a moving frame. This is what that cost and what
would fix it.

## What the tree has

`verify` renders sixteen named viewpoints, one frame each, and compares every pixel against a stored
reference. A change either moves a picture or it does not, and the report names which view and by
how much. It is exact, it is cheap, and it settled the specialized-kernel work in one run.

It cannot see anything that needs a second frame. A stale table copy, a row a placement missed, a
history reprojected onto the wrong surface — none of those exist in a single frame from a standing
camera, which is the only thing `verify` renders.

## What was used instead, and what it cost

For want of an oracle the moving path was judged by **the mean brightness of a frame**, counted over
`bench --suite=streaming --albedo`, with a swing of more than 40% against the frame before called a
defect. It began at 97 frames of 600 and reached 26.

That number is not an oracle. It cannot tell these apart:

- the picture changed because a table was stale,
- the picture changed because the camera moved,
- the picture changed because a cell arrived,
- the picture changed because the upscaler was handed a disocclusion.

So it was misread three times in one session, each time with a confident explanation attached: as a
double-buffer residue, as the camera flying into a hill, and as the streamer failing to keep up.
None of those survived the next measurement. What the evidence actually says about the 26 is written
in `ISSUES.md` and amounts to *unknown*.

**A scalar over a frame is not a test.** It has no expected value, so it can only be compared with
itself, and every comparison needs a story to interpret it.

## What would fix it

`verify`, but for a run rather than a view: a fixed schedule of frames through the streaming path,
each compared against a stored reference.

- **A schedule, not a clock.** The camera, the hour, the weather and the walk step by frame index,
  which `bench` already does — that is what makes a run reproducible today, and two runs of one
  build already agree.
- **Every frame compared, not a summary.** The report says which frames moved and by how much, the
  way `verify` names which view moved. A change is then read as "frames 267 to 274 differ", which is
  a place to look rather than a number to interpret.
- **References stored the way `verify` stores them**, so a run is a diff against a commit rather
  than against a memory of what the number was yesterday.

It answers the questions this session could not:

- Did this change the picture at all? — byte for byte, over a moving camera.
- Is this frame wrong, or is the world simply different here? — the reference says.
- Which frames does a defect touch? — named, not inferred from a swing.

## Cost, and what was built

`bench --hashes=<file>` writes one hash a frame; `bench --against=<file>` says which frames of this
run draw something else. A run is 660 lines of a few dozen bytes, against the few hundred megabytes
six hundred 1080p pictures would be. `FrameHashes` is the type; the hash is MurmurHash3 over the
pixels as the tool would write them to a PNG, so it names the picture a person would look at.

Reading a frame back submits a copy and waits on it, so a hashed run is serialised against the
device. The run says so where its times are printed, not only in `--help`.

## What it found on its first use

**No frame after the first was reproducible**, even at a standing camera: two runs of one binary
differed on 22 of 24 frames. Frame 0 was identical every time.

### The first cause — the exposure ran on the wall, and it is fixed

The scene the walk handed over was byte-identical every frame — instances, lights, sprites and
emitters all the same. So the difference was in the renderer, and there was one wall-clock input:

```cpp
const float sinceLastMs = std::chrono::duration<float, std::milli>(now - *mLastFrameAt).count();
```

The eye adapts in real time and the upscaler tunes itself against how fast a motion vector was
travelled, so a game is right to read the clock. A measured run is not: two runs of one build then
adapt by different amounts and draw different pictures. Everything else a run animates already steps
by the frame index for exactly this reason — the world, the sea, the sky roll, the sampler.

`FrameOptions::mSinceLast` is how a caller with a schedule states it, and every measured command
states `RtxTool::sStepRate`'s worth: `bench`, `shot`, `verify`, and the upscaler stability test —
which was itself letting the exposure read the wall while asserting that a still camera resolves to
a still picture. `verify` is the one that mattered most: it renders every view through one renderer,
so each view was adapting over however long the view before it took. Empty leaves the renderer timing itself, which is a window and the game. **A standing camera
is now reproducible with the exposure left to adapt**, and so is a streaming run up to its first
cell crossing.

### The second cause — the composite bake queue, and it is fixed

Past the first crossing a streaming run still differed, from frame 70 of 660 — nine frames after the
crossing at 61, and the crossings themselves agreed exactly for the first six.

**Not the terrain quad tree**, which was the obvious suspect and the wrong one: `QuadTreeWorld::collect`
resolves its view and loads every entry it names, in the calling thread. It is deterministic given a
view point.

It is `CompositeQueue`. A chunk arrives, its bake is queued, and a worker thread hands it back
whenever it finishes — so which frame takes it is that thread's answer rather than the schedule's,
and two runs draw different pictures from the crossing onwards.

`SceneUploader::setSettled` waits for the bakes a hand-over queued before it takes any. **The
per-frame bound is kept** — a settled run still takes `sCompositesPerFrame` and no more, so the
arrival pattern is the one the streaming path really has and only the thread's timing is gone. What
it costs is a stall at the crossing that queued the bakes, which is a trade a measured run makes and
a game does not. `bench` settles when it is hashing, which is already a run that is not a benchmark.

### Where it stands

```
island-crossing              660 frames, every one of them the same
```

Six hundred and sixty frames, a camera moving through nineteen cell crossings, identical between
runs. `verify`'s `island-crossing` is the one thing left, and it does not go through `bench`.

