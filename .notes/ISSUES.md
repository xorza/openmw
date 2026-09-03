# Open issues

- `RtxSceneExtractorTest.spritesAreReadAfterTheWalkRatherThanAsItPassesThem` fails under
  `--gtest_shuffle` on some seeds. Two sprite positions differ by one ulp, so the order the suite
  ran in reaches the emitter's clock.
