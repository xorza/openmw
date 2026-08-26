# Open issues

- An exterior's fog extinction is derived from a different distance in the two paths. The harness
  measures it over `Rtx::distantLandReach()` (`components/rtx/lightbuilder.cpp:185`); the game
  measures it over the ramp `MWRender::FogManager` built from `viewing distance`
  (`apps/openmw/mwrender/rtx/rtxrenderer.cpp`). At the shipped defaults those are 32768 and 7168
  units, so a screenshot and a played frame stand in different air.

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

- The mirror walk is `TRAVERSE_ALL_CHILDREN` (`components/rtx/sceneextractor.cpp:284`), and
  `osg::Sequence` both visits every child under that mode and advances its own clock only under
  `TRAVERSE_ACTIVE_CHILDREN`. A `NiFltAnimationNode` flipbook (`components/nifosg/nifloader.cpp:987`)
  is therefore traced as all of its frames standing in the same place at once, and never animates.

- The harness's exterior triangle census is sensitive to the binary's layout rather than to the
  content. Adding one unused `#include` to `components/rtx/lightbuilder.cpp`, with no other change
  anywhere, moves `openmw-rtxtool scene --view=balmora` from 2,882,873 triangles to 2,882,875 and
  `--view=ald-ruhn` from 3,079,829 to 3,079,831, while instances, meshes, materials, textures and
  lights all stay put. Interiors are unaffected; both cells that move are exteriors with paged
  distant-land objects. Each build is stable across runs, so the number is reproducible but not
  comparable across builds.

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

- The harness installs no `NifOsg::Loader::setHiddenNodeMask`, so a NIF node hidden at load carries
  a node mask of zero rather than the game's `Mask_UpdateVisitor` and no visitor reaches it — the
  update traversal included. A `NifOsg::VisController` on such a node
  (`components/nifosg/controller.cpp:388`) therefore never runs, so a node the content hides at load
  and animates visible later stays hidden for the life of an `openmw-rtxtool` run.
