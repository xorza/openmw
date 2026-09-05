# Open issues

- `runInfo` at `apps/rtxtool/main.cpp:336` builds its `Rtx::RendererOptions` with a designated
  initializer list that skips `mCacheDirectory`, which `components/rtx/renderer.hpp:90` declares
  between `mShaderDirectory` and `mWidth`. Every build of the harness warns
  `missing initializer for member 'Rtx::RendererOptions::mCacheDirectory'`.

- Three tests pass one second in a whole `Rtx*` run of `build-debug`:
  `RtxUpscaledFrameTest.anUpscaledFrameIsTheSameFrameLarger` at 3385 ms and
  `RtxUpscalerStabilityTest.aStillCameraResolvesToAStillPicture` at 2354 ms each build a renderer of
  their own, which `harness.hpp:129` measures at 700-870 ms for `createRenderer` and 900-1150 ms for
  the first `setScene`. `RtxVisibilityTest.theVolumeLightsTheAirUpToASurfaceWhereverInASliceItStands`
  at 1446 ms traces 48 frames of a 33-pixel square. The whole run is 567 tests in 26.0 s.

- Renderer tests skip when renderer construction raises `Rtx::Error`. `createVulkanRenderer` at
  `components/rtxvulkan/vulkanrenderer.cpp:1472` turns every one of them into a reason, and
  `RendererTest::SetUp` at `apps/components_tests/rtx/harness.cpp:243` turns any reason into
  `GTEST_SKIP`. `components/rtxvulkan/image.cpp:57` throws that type for an image format with no
  recorded texel size, which is a fault in this code rather than a machine that cannot run the
  backend. The process reports success after it skips every GPU regression.

- The fog volume disagrees with the analytic answer for an even layer in two places the closed form
  it replaced did not. A ray lying exactly on the water surface reads up to thirteen levels apart
  between a dry cell and a cell whose water is at nought. And a level ray toward a sun a quarter of
  the way up reads the sun's in-scattering a fifth over the closed form's
  `irradiance * phase * column * crossed`: 0.597 against 0.500. Both are in
  `apps/components_tests/rtx/visibility/fog.cpp` at lines 158 and 818, with their tolerances widened
  to 14 levels and 0.12 to say so, and both were already so for every exterior.

- Fog attenuation for particles at `components/rtxvulkan/shaders/lib/sprites.glsl:384` and for cloud
  shells at `components/rtxvulkan/shaders/lib/medium.glsl:166` reads one `fogExtinctionAt` at the
  midpoint of the view path, then takes `exp(-extinction * span)`. Opaque geometry at the same
  distance is attenuated by the volume instead, so an even exponential height layer attenuates the
  two differently. `fogColumn` at `components/rtxvulkan/shaders/lib/fog.glsl:452` states that
  layer's exact integral and has no caller anywhere in the tree, and the comment in `fogAlong` at
  line 698 names it as though a shader read it.

- Fog drops both moons' in-scattering below `FOG_SHAFT_FLOOR`, even when their irradiance is
  nonzero. `fogSourcesAlong` at `components/rtxvulkan/shaders/lib/fog.glsl:324` sets `mMoonlit` from
  that threshold, and `components/rtxvulkan/shaders/fogscatter.comp:232` reads the same flag both to
  skip the moons' shadow ray and to zero what they deliver.

- Translucent panes receive water and fog attenuation over the distance to the surface behind them,
  rather than the distance to the pane. `components/rtxvulkan/shaders/visibility.rgen:166`.

- Faded actors peel only their nearest translucent surface. Clothing and body layers under it render
  at full strength. `components/rtxvulkan/shaders/visibility.rgen:123`.

- Exterior cell crossings hold the tail of the moving-camera benchmark. Three `build-release` runs of
  `bench --views=island-crossing --seconds=10` on a hot card measure a median frame of 6.7-7.2 ms
  against a p99 of 127-140 ms and a worst frame of 178-202 ms. The 19 crossings take 1.9 s of the
  8.6 s run, 1.2-1.3 s of it reading and 0.6 s building, and the worst single crossing is 175-193 ms.
  None of them is a rebuild. The cost is on the host: `walk` reaches 53-89 ms and `place` 27-30 ms in
  a frame whose whole device time is near 12 ms.

- The `gpu ms` row of a bench report gives each zone the median of the frames that ran that zone, so
  the zones cover different frame counts. The row cannot be summed, and no zone can be set against
  the frame median beside it. The `island-crossing` run prints `micromap 7.51` above a frame median
  of 6.73 ms, because the micromap pass runs at a crossing rather than every frame.
