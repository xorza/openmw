# Open issues

- The first-person arms trace at the world's field of view: `NpcAnimation`'s
  `OverrideFieldOfViewCallback` is a cull callback and never runs, so a `first person field of
  view` that differs from `field of view` is ignored.
- The debug render modes (`tcg`, `tpg`, `tnm`, actor paths, recast mesh, cell borders) draw
  nothing: their nodes hang on the world root above the scene root the walk starts from, and
  they are line geometry.
