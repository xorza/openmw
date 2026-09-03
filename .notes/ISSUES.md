# Open issues

- `RtxMoonBuilderTest` fails under `--gtest_shuffle` on some seeds. Its `seed()` plants `Moons_*`
  through `Fallback::Map::init`, which keeps whichever value arrives first, so a test that reads
  those keys before it wins.
