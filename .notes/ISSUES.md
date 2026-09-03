# Open issues

- `bench --hashes` and `--against` compare exact frame hashes across processes, and the driver
  finishes an acceleration structure some time after the build that made it: a structure starts
  answering the same rays with hit distances a few ulps away, which the trace turns into a different
  sample on a few hundred pixels. The frames a run traces before that lands hash differently from a
  run where it landed earlier. `verify` watches a view for the crossing, keeps the picture from
  both sides of it and compares a run with whichever it drew; `bench` has no such step.

- `verify` accepts `--albedo` and ignores it: the picture it draws is the shaded one whether or not
  the flag is given, where `shot --albedo` draws the albedo.

- The staged scene's vertex order is not stable across processes: in one of twenty-four stagings of
  `balmora-mages-guild`, two eight-vertex meshes came out with their vertex runs swapped — the
  positions and texture coordinates were a permutation of every other staging's, and the instances
  were identical.
