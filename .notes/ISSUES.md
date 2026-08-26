# Open issues

- Auto-exposure has no time constant. The histogram is measured on the frame the curve is about to
  map and the result is applied to that same frame (`components/rtxvulkan/vulkanrenderer.cpp:739`),
  with nothing carried between frames, so the picture's brightness is a pure function of what is on
  screen this frame.

- `MWMechanics::Actors` (`apps/openmw/mwmechanics/actors.cpp:1243`) sets an actor's base node mask
  to zero beyond `actors processing range` and back on the frame after, so an actor oscillating
  across that distance takes its carried light in and out of the walk a frame at a time.

- `Rtx::Instance`'s constructor comment (`components/rtxvulkan/instance.cpp:182`) says a renderer
  that fails to start leaves "the game carries on with OpenGL". `MWRender::RtxRenderer` throws on a
  null backend (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:184`), and with the ray tracer on no GL
  context exists to carry on with.
