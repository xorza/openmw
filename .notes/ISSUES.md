# Open issues

- Three tests pass one second in a whole `Rtx*` run of `build-debug`:
  `RtxUpscaledFrameTest.anUpscaledFrameIsTheSameFrameLarger` at 3385 ms and
  `RtxUpscalerStabilityTest.aStillCameraResolvesToAStillPicture` at 2354 ms each build a renderer of
  their own, which `harness.hpp:129` measures at 700-870 ms for `createRenderer` and 900-1150 ms for
  the first `setScene`. `RtxVisibilityTest.theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands`
  at 1446 ms traces 48 frames of a 33-pixel square. The whole run is 567 tests in 26.0 s.

- `RtxVisibilityTest.aSpriteIsShadowedByWhatStandsOverIt` turns on which numbers the seed constants
  in `random.glsl` happen to hold. The froxel's ambient term is one direction per froxel per frame —
  `ambientReaching` over the whole sphere, at `AMBIENT_EXTERIOR_RATE` — and the fixture draws one
  frame, so the lidded sprite is a handful of Bernoulli draws. Moving `SEED_LAMPS_MIRROR` and the
  seeds after it by three took the lidded value from under 5% of the open one to 92% of it.

- Exterior cell crossings hold the tail of the moving-camera benchmark. Three `build-release` runs of
  `bench --views=island-crossing --seconds=10` on a hot card measure a median frame of 6.7-7.2 ms
  against a p99 of 127-140 ms and a worst frame of 178-202 ms. The 19 crossings take 1.9 s of the
  8.6 s run, 1.2-1.3 s of it reading and 0.6 s building, and the worst single crossing is 175-193 ms.
  None of them is a rebuild. The cost is on the host: `walk` reaches 53-89 ms and `place` 27-30 ms in
  a frame whose whole device time is near 12 ms.
