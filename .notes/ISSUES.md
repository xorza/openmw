# Open issues

- An exterior's fog extinction is derived from a different distance in the two paths. The harness
  measures it over `Rtx::distantLandReach()` (`components/rtx/lightbuilder.cpp:185`); the game
  measures it over the ramp `MWRender::FogManager` built from `viewing distance`
  (`apps/openmw/mwrender/rtx/rtxrenderer.cpp`). At the shipped defaults those are 32768 and 7168
  units, so a screenshot and a played frame stand in different air.

- Auto-exposure has no time constant. The histogram is measured on the frame the curve is about to
  map and the result is applied to that same frame (`components/rtxvulkan/vulkanrenderer.cpp:739`),
  with nothing carried between frames, so the picture's brightness is a pure function of what is on
  screen this frame.

- The mirror walk is `TRAVERSE_ALL_CHILDREN` (`components/rtx/sceneextractor.cpp:284`), and
  `osg::Sequence` both visits every child under that mode and advances its own clock only under
  `TRAVERSE_ACTIVE_CHILDREN`. A `NiFltAnimationNode` flipbook (`components/nifosg/nifloader.cpp:987`)
  is therefore traced as all of its frames standing in the same place at once, and never animates.

- A `LIGH` record flagged `Negative` is dropped by `Rtx::makeLight(const ESM::Light&)` as something
  a ray tracer cannot express, but the game's scene graph builds one anyway:
  `SceneUtil::createLightSource` (`components/sceneutil/lightutil.cpp:130`) negates the diffuse
  instead, so the walk mirrors a light of negative intensity where the harness places none.

- `RenderingManager::toggleRenderMode(Render_Scene)` — the `tws` console command
  (`apps/openmw/mwrender/renderingmanager.cpp:746`) — hides the world by flipping `sToggleWorldMask`
  in the master camera's cull mask, which the RTX path never reads. The water half goes through
  `Water::showWorld` and works; the rest of the world stays traced.

- `MWMechanics::Actors` (`apps/openmw/mwmechanics/actors.cpp:1243`) sets an actor's base node mask
  to zero beyond `actors processing range` and back on the frame after, so an actor oscillating
  across that distance takes its carried light in and out of the walk a frame at a time.

- `Rtx::Instance`'s constructor comment (`components/rtxvulkan/instance.cpp:182`) says a renderer
  that fails to start leaves "the game carries on with OpenGL". `MWRender::RtxRenderer` throws on a
  null backend (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:184`), and with the ray tracer on no GL
  context exists to carry on with.
