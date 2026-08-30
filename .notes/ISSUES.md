# Open issues

- `RtxRetireTest.aCompactedSceneRendersAsOneThatNeverLostAnything` passes under
  `--gtest_filter='Rtx*'` and fails under a narrower filter, with a worst channel difference of 4
  against a tolerance of 2. Its result depends on which tests ran before it in the same process.
