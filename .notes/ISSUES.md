# Open issues

- An exterior's fog extinction is derived from a different distance in the two paths. The harness
  measures it over `Rtx::distantLandReach()` (`components/rtx/lightbuilder.cpp:185`); the game
  measures it over the ramp `MWRender::FogManager` built from `viewing distance`
  (`apps/openmw/mwrender/rtx/rtxrenderer.cpp`). At the shipped defaults those are 32768 and 7168
  units, so a screenshot and a played frame stand in different air.

- The update traversal walks the whole scene twice a frame. `RtxRenderer::updateTraversal`
  (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:332`) accepts the update visitor on the master camera
  to reach `MWRender::Camera`'s callback, and `Stage::adopt` (`apps/openmw/mwrender/stage.cpp:82`)
  has parented the scene root under that camera — so `UpdateRenderCameraCallback`'s `traverse`
  descends the whole world again at the same traversal number. Every animation controller, every
  `LightController` and `LightManager::update` runs twice per frame, and on the second pass the node
  path starts at an `ABSOLUTE_RF` camera, so the view matrix is folded into each light's world
  transform.

- `WindowManager::enableScene` (`apps/openmw/mwgui/windowmanagerimp.cpp:629`) blanks the traversal
  mask of the update visitor the RTX renderer owns, while the mirror walk keeps its own
  `sWorldTraversal`. While it is in effect the update reaches no scene node and the mirror still
  reads what the update no longer refreshes.

- Glow lights carry their colour in the ambient term — `Animation::setLightEffect`
  (`apps/openmw/mwrender/animation.cpp:1920`) sets a zero diffuse and a bright ambient — and
  `SceneExtractor::addLight` (`components/rtx/sceneextractor.cpp:741`) reads only the diffuse. Light
  spells and enchanted items light nothing in the RTX path.

- `sWorldTraversal` (`apps/openmw/mwrender/rtx/rtxrenderer.cpp:83`) does not exclude
  `Mask_UpdateVisitor`, which `RenderingManager` installs as `NifOsg::Loader`'s hidden node mask, so
  NIF nodes the game marks hidden are mirrored and traced.

- `mTarget` is discarded from `VK_IMAGE_LAYOUT_UNDEFINED` with a `TOP_OF_PIPE` source scope at the
  top of every frame (`components/rtxvulkan/vulkanrenderer.cpp:634`) while the presenter's blit out
  of it may still be running: `Presenter::present` submits asynchronously and waits its fence only
  when that swapchain image comes round again (`components/rtxvulkan/presenter.cpp:175`).

- `mHistoryStale` is cleared at the end of every frame (`components/rtxvulkan/vulkanrenderer.cpp:767`)
  whether or not anything read it. With no upscaler and no wavelet neither consumer runs, so a
  `resetHistory` is dropped rather than deferred.

- Auto-exposure has no time constant. The histogram is measured on the frame the curve is about to
  map and the result is applied to that same frame (`components/rtxvulkan/vulkanrenderer.cpp:739`),
  with nothing carried between frames, so the picture's brightness is a pure function of what is on
  screen this frame.

- `Animation::setObjectRoot` (`apps/openmw/mwrender/animation.cpp:1545`) tears off the subtree that
  owns `mExtraLightSource` and never re-adds it, so an actor whose object root is rebuilt loses its
  carried light permanently.

- `LightSource::getEmpty` means "the model this light hangs on has no geometry", decided once by
  `CheckEmptyLightVisitor` (`components/sceneutil/lightutil.cpp:106`) — and
  `SceneExtractor::addLight` (`components/rtx/sceneextractor.cpp:730`) reads it as a reason to drop
  the light. A `LIGH` record with an empty mesh is therefore never lit in the RTX path.

- The mirror walk is `TRAVERSE_ALL_CHILDREN` (`components/rtx/sceneextractor.cpp:280`), so
  `osg::Switch::traverse` visits every child — a light under the branch `DayNightCallback`
  (`apps/openmw/mwrender/animation.cpp:107`) switched off is extracted where the rasterizer draws
  nothing.

- `MWMechanics::Actors` (`apps/openmw/mwmechanics/actors.cpp:1243`) sets an actor's base node mask
  to zero beyond `actors processing range` and back on the frame after, so an actor oscillating
  across that distance takes its carried light in and out of the walk a frame at a time.
