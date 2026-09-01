# Open issues

- Staging a cell a second time in one process renders a different frame from the first. `verify
  --views=arkngthand,arkngthand` reports the first frame identical to a run of `--views=arkngthand`
  and the second `differs: worst 74 of 255 on 54.46% of the pixels`. It does not depend on which
  cell was staged before: any second staging differs, and by about the same amount. The difference
  is scattered speckle over lit surfaces with no structural change, and it survives `--exposure=1`,
  `--props=false` and `--delight=0`.
