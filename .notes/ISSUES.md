# Open issues

- `runInfo` in `apps/rtxtool/main.cpp` builds its `Rtx::RendererOptions` with a designated
  initializer list that skips `mCacheDirectory`, so every build of the harness warns
  `missing initializer for member 'Rtx::RendererOptions::mCacheDirectory'`.

- A layer of puffs takes its three visibility rays at whichever sprite or shell the walk reached
  first and applies that one answer to every puff in the layer, so where the layer straddles a
  shadow edge the answer flips between neighbouring pixels and the silhouette of that one puff is
  drawn into the picture. Forcing `PuffAbove::mSunLit` to one removes the symptom outright at both
  places it shows: the drain splash at Vivec (cell `2,-10` at 18752, -81735, 95, bearing 117°, climb
  -11°) and the blight cloud at Dagoth Ur (cell `2,8` at 19247, 70446, 13757, bearing 128°, climb
  -27°, weather Blight). Neither is the upscaler; `--upscale=off` shows both.

- A puff the layer's rays find shadowed has no light left at all. `puffLight` gates its sun, its sky
  and its lamps on a visibility apiece and carries no indirect term, where every surface in the
  frame is rescued by its bounce — so a splash under a canton lip and a cloud in the shadow of a
  caldera wall both go to black rather than to a dimmer shade of themselves.
