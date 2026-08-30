# Open issues

- `apps/rtxtool/bench.cpp` reports a `place ms` row that times only `SceneUploader::hand` after a
  motion step. The walk that precedes it, `PosedActors::step`, is inside `frame ms` and in no row of
  its own. In the streaming suite that walk is the largest CPU cost of a frame (perf: 23.5% of all
  samples, about 6.9 ms a frame on average) and the report has no number for it.
- `apps/rtxtool/bench.cpp` `place ms` includes the fence wait in `VulkanRenderer::beginFrame` when
  the frame ring is full, because `hand` runs before `finishFrame` in the loop. At 3840x2160 the row
  reads 8.3 ms median against 1.8 ms at 1920x1080 for the same placement work.
- `components/rtxvulkan/sceneacceleration.cpp` `extend` records the BLAS builds a crossing brings
  without a GPU zone, so a crossing frame's device time is spread over the zones that were open
  and the build itself appears nowhere in the bench's `gpu ms` row.
- `docs/source/reference/modding/settings/rtx.rst` documents `enabled` and `upscale`. The `[RTX]`
  section of `files/settings-default.cfg` also carries `distant land cells` and `preset`, and
  neither has an entry.
