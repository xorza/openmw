# Open issues

- Two frames in flight draw pictures that differ from run to run. Two unheld `bench
  --views=one-cell-walk --seconds=6 --window=false --upscale=off --filter=false --validation=off`
  legs, each hashing every frame through the ring's own readback (`FrameOptions::mReadBack`)
  without waiting a frame out, differ on 187 to 219 of 360 frames while every scene column
  agrees; two legs that wait each frame out before the next is placed agree on all 360, and so
  do two held (`--hold=8`) legs. Synchronization validation reports nothing on an unheld hashed
  leg. Dumped at 1920×1080 with `--exposure=1`, the differing frames are of two kinds: the sky
  differing by one in 255 over about 8% of its pixels, and the lower half of the picture
  differing by up to 168 on a few frames. A played game runs with two frames in flight.
- `components-tests` does not build in `build-release/`: GCC 16 at `-O3` refuses
  `apps/components_tests/rtx/slottable.cpp` with `-Werror=stringop-overflow=` ("writing 8 bytes
  into a region of size between 1 and 3") from `std::vector<std::uint8_t>::resize` inlined
  through `SlotSet::grow` → `RowDebt::owe` → `SlotTable::write` in
  `syncingWithoutGrowingLeavesTheBufferWhereItIs`. The gate builds only the two libraries in
  release, so nothing runs the tests at `-O3`.
