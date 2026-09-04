# Open issues

- Renderer tests include shared renderer and pipeline construction in individual test durations.
  After shader changes, `RtxGpuTimerTest.aFrameAccountsForItsOwnDeviceTimePassByPass` and
  `RtxFrameCostTest.aWarmRendererDrawsAStillFrameWithoutTheHeap` exceed the one-second limit.

- Fog attenuation for particles in `sprites.glsl` and cloud shells in `medium.glsl` uses a single
  midpoint density over the whole view path. Even a uniform exponential height layer therefore
  attenuates those layers differently from the opaque geometry at the same distance.

- Renderer tests skip when renderer construction raises an internal error, including an unrecognized
  image format. The test process can report success after skipping every GPU regression.

- Fog drops both moons' in-scattering below `FOG_SHAFT_FLOOR`, even when their irradiance is
  nonzero. The threshold determines whether to cast their shadow ray and also removes their energy.

- Translucent panes receive water and fog attenuation over the distance to the surface behind them,
  rather than the distance to the pane.

- Faded actors peel only their nearest translucent surface; underlying clothing and body layers
  render at full strength.

- Crossing an exterior cell boundary near the Seyda Neen ship produces frames over 120 ms in
  the headless moving-camera benchmark, including with the previous fog implementation.
