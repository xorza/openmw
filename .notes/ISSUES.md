# Open issues

- During a weather transition the ray tracer turns both cloud sheets by one bearing. The
  rasterizer turns each cloud mesh by its own weather's storm direction, which
  `WeatherResult::mNextStormDirection` carries and `Shaders::CloudDeck` has no field for.

- The harness crosses the cloud deck linearly across a transition. `RtxTool::applyLighting` hands
  `describeClouds` the weather's own blend, and `CellLighting::mCloudBlend` — documented as not
  being that number — is never written and never read.

- `Shaders::VisibilityConstants` carries `mWindSpeed` and `mStormDirection` to the GPU, and no
  shader reads either.
