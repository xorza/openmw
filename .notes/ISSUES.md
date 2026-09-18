# Open issues

- `rtx debug gate`'s repeat pair differs on the picture on the first gate after a rebuild, and
  on no other run: two of four gates on 2026-09-18 (21 and 59 of 360 pictures, scattered from
  frame 223 and from frame 13, every scene column the same), both the first gate run after
  `build`, `compileWithoutAsserts` and `compileWithoutDlss` had work to do; the two gates run
  again with nothing to build were identical, and so were twenty-four standalone pairs of
  `rtx debug repeat` the same day, including two from a cold pipeline cache and two straight
  after `check --validation=sync`. The second leg's hashes are not kept by `runRepeat`, so
  which pixels differed is not known.
