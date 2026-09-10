# Open issues

- `bench --views=seyda-neen-ship` at `--size=1280x720` spreads from 0.73 to 1.04 ms on the `trace`
  zone across legs of the same build at a held clock of 2220 MHz, while the guild and Arkngthand
  repeat to a hundredth. The 1080p legs at the ship spread the same way on every batch of the day.

- The ray tracer does not read the terrain's vertex colours. `Terrain::Storage::fillVertexBuffers`
  fills the land record's `VCLR` beside the heights and the rasterizer multiplies its ground by
  them; `Rtx::MeshReading` carries no colour and no scene table holds one, so the ground is shaded
  as though every vertex were white.
