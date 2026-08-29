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

## Cost

A streaming run is 600 frames at 1920x1080. Stored as PNG that is a few hundred megabytes, which is
too much to keep beside the sixteen stills. Two ways out, and the first is probably right:

- **A hash a frame.** The reference is 600 hashes, a few kilobytes. It answers "did it change" and
  names the frames, and a frame that changed is then rendered on demand for a look. It cannot say
  *by how much*, which `verify`'s report does say.
- **A smaller extent.** The same schedule at 480x270 is a fortieth of the pixels and still shows a
  chunk of terrain going dark. It keeps the full report and the diff, and it is a different picture
  from the one a player sees.

The hash is the cheaper start and it is enough for every question above but the last.
