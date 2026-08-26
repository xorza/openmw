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

- `RtxTool::World::buildTerrain` (`apps/rtxtool/world.cpp:270`) unions each arriving cell into
  `mActiveGrid` and never narrows it, so the grid handed to `Terrain::World::setActiveGrid` is every
  cell the run has ever loaded. `dropCellsOutside` (`apps/rtxtool/cellscene.cpp:121`) meanwhile keeps
  only the 3×3 square around the centre. A cell between the two is in neither picture:
  `ObjectPaging::getChunk` (`components/terrain/objectpaging.cpp:39`) returns nothing for a chunk the
  quad tree marked active-grid, and the harness builds it with `pageActiveGrid=false`. The ground
  survives because `Terrain::ChunkManager` makes no such refusal, so a camera that moves leaves a
  corridor of ground with no statics on it.
