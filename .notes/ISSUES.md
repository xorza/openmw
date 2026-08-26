# Open issues

- `MWMechanics::Actors` (`apps/openmw/mwmechanics/actors.cpp:1243`) sets an actor's base node mask
  to zero beyond `actors processing range` and back on the frame after, so an actor oscillating
  across that distance takes its carried light in and out of the walk a frame at a time.

- `Rtx::Instance`'s constructor comment (`components/rtxvulkan/instance.cpp:182`) says a renderer
  that fails to start leaves "the game carries on with OpenGL". `MWRender::RtxRenderer` throws on a
  null backend (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:184`), and with the ray tracer on no GL
  context exists to carry on with.
