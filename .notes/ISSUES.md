# Open issues

- `openmw-rtxtool shot --tail` aborts on `AccumulatePass::getBlended`'s
  `!mFresh` assert, with or without `--accumulate`. Reproduced on a clean
  `74d6702ce1`.

- A run a played binary starts from `[RTX] session` writes a full report that
  nothing reads: `MWRender::Session` leaves it in the result slot, and only
  `apps/rtxtool` ever takes one. Nothing reaches the log either.
