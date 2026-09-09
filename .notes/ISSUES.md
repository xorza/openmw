# Open issues

- Four comments name `.notes/` files that the tree does not hold:
  `components/rtx/distantlights.cpp:113` names `.notes/rtx/performance.md`,
  `apps/components_tests/rtx/memory.cpp:193` and
  `apps/components_tests/rtx/deviceprofile.cpp:131` name `.notes/design-vulkan.md`, and
  `apps/components_tests/rtx/visibility/filter.cpp:190` names `.notes/rtx/shader-review.md`.

- `bench --views=island-crossing,seyda-neen-ship,vivec` never leaves the first stop. The camera
  keeps flying past the route's end and cells keep loading; the run drew about 128,000 frames in ten
  minutes before it was killed. The same three views without the route one finish normally, and
  `--views=island-crossing` alone finishes normally.
