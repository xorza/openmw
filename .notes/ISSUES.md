# Open issues

- `bench --views=seyda-neen-ship` at `--size=1280x720` spreads from 0.73 to 1.04 ms on the `trace`
  zone across legs of the same build at a held clock of 2220 MHz, while the guild and Arkngthand
  repeat to a hundredth. The 1080p legs at the ship spread the same way on every batch of the day.

- The ray tracer does not read the terrain's vertex colours. `Terrain::Storage::fillVertexBuffers`
  fills the land record's `VCLR` beside the heights and the rasterizer multiplies its ground by
  them; `Rtx::MeshReading` carries no colour and no scene table holds one, so the ground is shaded
  as though every vertex were white.

- `RtxGuiDrawTest.aPictureInsideTheInterfaceLeavesTheFramesExposureAlone` fails intermittently. One
  run of `components-tests --gtest_filter='Rtx*'` had the frame read back at 18 of 255 where the
  test expected the 17 it carried before the picture; the same filter passed 604 of 604 on the three
  runs after it, and the test alone passed five times in a row.
