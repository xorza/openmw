# Open issues

- `bench --hashes` frames drawn after a cell arrives mid-run stand on structures the driver settles
  at a random later frame, the way `watchSettling` describes for a scene's first build; a run
  compared with `--against` reports the frames between the two runs' settlings as different.

- The staged scene's vertex order is not stable across processes: in two of about two hundred
  `verify` stagings of `balmora-mages-guild`, the positions and texture coordinates came out as a
  permutation of every other staging's — once seen as two eight-vertex meshes with their vertex runs
  swapped, the instances identical — and ninety `scene` stagings of the same cell never did.
