# Open issues

- A reorder mode changes what the trace computes. `verify --exposure=1 --upscale=off --filter=false`
  with `--reorder=hint` against `--reorder=off` differs at five of twenty-two views, worst 50 of 255
  at `balmora-mages-guild`. Each mode is repeatable against itself and `off` is repeatable against
  itself, so it is a difference and not noise. `lib/fog.glsl` records one site of this kind already
  found and fixed with an explicit `textureLod`; at least one more remains.

- `repeatable.sh` cannot see a picture change that a reorder mode makes. It walks `one-cell-walk`,
  which is one of the views that does not move, and forces `--upscale=off --filter=false`.
