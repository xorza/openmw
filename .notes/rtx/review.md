# Review of the RTX fork against `upstream/master`

Merge base `d720b4c82ccfa1f4d89353699929545945d79a2c`. The diff is 615 files, +92145/−4906.
The RTX-owned places hold about 385 files and 79,900 lines. The upstream-owned places hold 216
files, +10257/−4844.

Delete an item when it is addressed. Delete a heading when it is empty. This file lists open items
only.

Each item describes what is there and what it costs. No item says how to fix it.

## Two constants that must differ are equal, and a sum that drops fields

- [ ] `components/rtxvulkan/shaders/lib/random.glsl:21` `SEED_LAMPS_PANE = 0x53u` and line 26
      `SEED_LAMPS_MIRROR = 0x53u`. The comment above the second says "Two constants and not one,
      because two reservoirs seeded alike step the same sequence and keep the same lamps." A pane
      in front of water (`visibility.comp:98`) and the water's reflection reservoir
      (`water.glsl:267`) draw from one sequence.
- [ ] `components/rtx/sceneextractor.cpp` `ExtractionStats::operator+=` sums thirteen fields and
      omits `mSheets` and `mComposites`.
      `apps/rtxtool/scene.cpp:498-499` adds the settled walk to the staged walk and prints
      `mComposites` and `mSheets`. The settled walk's composites and sheets are missing from the
      report.

## Work is done per frame that the frame did not change

- [ ] `components/rtx/sceneextractor.cpp` increments
      `stats.mTextureFormats[describeFormat(*pending.mSprite)]` per emitter per frame. This builds
      a `std::string` and inserts into a `std::map` on the frame path. `getTextureTransform`
      builds `std::string(sNames[unit])` per pass. `readMask` calls `image.getColor` per texel.
- [ ] `components/rtx/moonbuilder.cpp` `makeMoon` constructs `Sky::MoonModel(nameOf(moon))` on
      every call. The constructor does fallback-map string lookups. `rtxrenderer.cpp` and
      `apps/rtxtool/lighting.cpp` call it twice per frame.
- [ ] `components/rtx/scenedesc.cpp` `release()` allocates two `std::vector<char>` on every sweep
      frame. `setMaterial` reclassifies by a scan over every instance. `forEachPlacement` allocates
      a per-mesh box vector on every `getBounds` and `getContentBoundsWithin`.
- [ ] `components/rtx/frametimes.hpp` holds `std::vector<std::vector<double>> mTimes`, one
      allocation per inner zone.
- [ ] `components/rtxvulkan/scenebuffers.cpp` `place` refills `mLightScratch`, `mSpriteScratch`
      and `mEmitterScratch`, calls `mLightGrid.rebuild`, and writes the light, offset, index, grid
      and emitter tables whole on every placement. `SlotTable` tracks changed rows for instances and
      materials. The lights have no such account, so a still cell with three hundred lamps rewrites
      and re-bins them every frame.
- [ ] `components/rtxvulkan/guitextures.*` is synchronous. `getView`, `read` and `writeWith` all
      call `flush()`, which is a submit and a wait. `drawGui` calls `getView` per batch. A texture
      written in a frame costs one submit-and-wait inside that frame. `videowidget.cpp` writes one
      texture per frame.
- [ ] `components/rtxvulkan/vulkanrenderer.cpp:894-897` and `finishOldest` map and unmap
      `Frame::mHitCount` twice per frame when counting. `Buffer::map` calls `vkMapMemory` on every
      call.
- [ ] `components/rtxvulkan/sceneacceleration.cpp:268-269` `writeGeometry` calls
      `mPositions.reserve` once per slot, and `SlotBlocks::reserve` already loops over every slot.
      The reservation runs `mSlots²` times.
- [ ] `apps/rtxtool/world.cpp` `findCell` scans every cell record per call. `cellscene.cpp` calls
      it for each of the nine squares on every crossing and once per departed cell.
      `apps/rtxtool/objectstorage.cpp` builds an exterior index over the same data.
- [ ] `apps/rtxtool/stagedworld.cpp` walks the graph twice in the constructor: `mirror(0)` at line
      439, then again at line 482 or through `mPosed->settle()`. The first walk's result is
      discarded on the actor path.

## The same number or the same arithmetic is written in more than one place

- [ ] The sRGB decode exists three times: `components/rtx/lightbuilder.cpp` `channelToLinear`,
      `components/rtx/texelreader.cpp` `toLinear`, `components/rtx/shadingmap.cpp` anonymous
      `toLinear`.
- [ ] `apps/rtxtool/picture.cpp:40-47,219` restates `apps/openmw/mwrender/localmap.cpp:40-42,169-170`
      (`50000`, `5`, `150000`, light `(-0.3, -0.3, 0.7)`, diffuse `0.7`, ambient `0.3`) and
      `characterpreview.cpp:69` (`12.3f`, the `700`/`71` doll camera). A change to the game's map
      or doll makes `rtxtool map`/`doll` a different picture with nothing to say so.
- [ ] `apps/rtxtool/objectstorage.cpp` `collectReferences` and `apps/rtxtool/world.cpp`
      `forEachObject` both implement the moved-ref, leased-ref and deleted-ref reduction over
      `cell.mContextList`, each with its own `typeOf` and `departed` lambdas.
      `apps/openmw/mwrender/objectstorage.cpp` is a third copy of the same reduction.
- [ ] `apps/rtxtool/lighting.cpp` `applyLighting` and `apps/openmw/mwrender/rtx/rtxrenderer.cpp`
      `traceWorld` each assemble an `Rtx::FrameWorld` from the same inputs: `describeStars`,
      `skyBudget`, `makeMoon` for both moons, `fogColour`, `describeClouds` with `deckLight` and
      `stormDirection`, `describePatches`. Two assemblies of one frame.
- [ ] `components/rtxvulkan/vulkanrenderer.cpp` builds `SceneStats` with the same eight designated
      initialisers in `setScene` (435-444) and `extendScene` (497-506), and assigns five of them by
      hand in `placeScene` (600-604).
- [ ] `components/rtxvulkan/commands.cpp` `CommandPool::submit` (56-94) and `endAndWait` (141-175)
      both build `mSubmitScratch` from `mDeferred` plus one buffer and both clear the three deferred
      lists.
- [ ] `Buffer`, `HostBuffer`, `Texture`, `ShaderModule` and `DeviceMemory` each write the same
      move constructor, move assignment and `destroy` by hand. `Buffer::write` maps and unmaps per
      write; `HostBuffer` stays mapped. Two buffer classes serve one job.
- [ ] `components/rtxvulkan/texture.cpp` `Texture` owns its own `VkImage`, view and memory and a
      local `barrier` lambda (150-172) that restates `Image::transitionLevels`. `Image` exists.
- [ ] `components/rtxvulkan/sceneacceleration.cpp` `buildMeshes` (654-678) and `prepareRefit`
      (851-868) build the same `VkAccelerationStructureGeometryKHR` triangles block, with the same
      "Guarded, because a freed slot has no vertices" comment word for word.
- [ ] `components/rtxvulkan/shaders/lib/fog.glsl` `fogWeatherAlong` (429-438) and
      `fogUniformAlong` (700-714) each write the per-stretch sun and moons block: the probe,
      `lightThrough`, `fogBeamDepth` for the sun and for each moon, `daylightReaching`.
- [ ] `components/rtxvulkan/shaders/lib/sea.glsl:183` `pixelKey(uvec2(cell)) * 0x9E3779B9u`
      restates the first half of `randomSeed`.
- [ ] `components/rtxvulkan/visibilitypass.cpp:307-324` names bindings `13`..`19` and
      `sFrameBinding + 1..3` as literals. `bindings.glsl` declares the same numbers. The two are
      kept in step by hand and by one `assert(filled == writes.size())`.
- [ ] `apps/rtxtool/main.cpp:663-670` builds `ActorRequest` by hand where `actorsFrom(variables)`
      (line 217) exists. Lines 623-627 and 676-680 both spell the "was validation asked for" test.
- [ ] `ShotRequest`, `ViewRequest`, `BenchRequest` and `VerifyRequest` each carry the same block
      (shader directory, size, field of view, upscale, preset, delight, filter, exposure, weather,
      hour, day, actors). `main.cpp` `dispatch` fills the block four times by hand (601-618,
      637-670, 705-726, 731-762).
- [ ] The harness parses floats two ways: `std::stof` in `views.cpp:23` (locale-dependent) and
      `std::istringstream` with the classic locale in `placement.cpp:81`. `placement.cpp:76`
      says `from_chars` is unavailable for floats on libc++; `main.cpp:120` and `world.cpp:368`
      use `from_chars` for integers.
- [ ] `apps/rtxtool/lighting.hpp` `CellLighting` carries `mAmbient` and `mFog` beside
      `mDaylight`, which already holds both. `lighting.cpp` `settle` copies them across.
- [ ] `apps/openmw/mwrender/rtx/tracedview.cpp` keeps its own `sNextName` counter for MyGUI texture
      names. `MyGUIPlatform::Picture` has a naming scheme for the same purpose.

## Requirements and code that nothing uses

- [ ] `components/rtxvulkan/device.hpp` `DeviceFunctions` loads `vkCreateRayTracingPipelinesKHR`,
      `vkGetRayTracingShaderGroupHandlesKHR`, `vkCmdTraceRaysKHR`,
      `vkCmdCopyAccelerationStructureKHR` and `vkCmdWriteAccelerationStructuresPropertiesKHR`. No
      file outside `device.*` names them. `device.cpp:26` throws if any is missing.
- [ ] `components/rtxvulkan/requirements.cpp` requires `VK_KHR_ray_tracing_pipeline`,
      `VK_EXT_ray_tracing_invocation_reorder` and the features `rayTracingPipeline`,
      `rayTraversalPrimitiveCulling`, `rayTracingInvocationReorder`, `timelineSemaphore`,
      `hostQueryReset`, `samplerAnisotropy`. The renderer is compute with ray queries. No shader
      names a reorder hint. Every sampler sets `anisotropyEnable = VK_FALSE`. Query pools are reset
      on the command buffer. A device without one of these is refused for a capability nothing
      uses. The optional `VK_NV_cluster_acceleration_structure` and
      `VK_NV_partitioned_acceleration_structure` are enabled and unused.
- [ ] `apps/rtxtool/view.cpp:50-65` `OnScopeExit` is defined and never instantiated.
- [ ] `apps/rtxtool/cellscene.cpp` `readRegion` and `dropCellsOutside` take `Rtx::SceneDesc& scene`
      and `Rtx::SceneExtractor& extractor` and never read them. `loadRegion` passes them through.
- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.cpp` calls `addMoonFaces` and `addSkyContent` once.
      `dropMoonFaces` and `dropSkyContent` have no caller in the game.
- [ ] `components/rtxmetal/metalrenderer.mm` is a factory that always returns null with a reason.
      `components/rtxmetal/CMakeLists.txt` compiles `shaders/visibility.metal` for it.

## Two paths for one job, and asymmetric interfaces

- [ ] `components/rtxvulkan/vulkanrenderer.cpp` `placeScene` has two bodies: a view scene runs
      `submitAndWait` (535-546), the world defers into the frame (548-598). `traceGuiTexture`
      (1178-1306) restates `renderFrame`'s `VisibilityInputs` block (ten fields) and the
      accumulate, filter, composite and tone chain with different images.
- [ ] `SceneAcceleration::place` returns whether it recorded; `SceneBuffers::place` returns
      nothing. `SceneBuffers::getBytes` omits `mSpriteTileOffsets` and `mSpriteTileIndices`
      although `Tables` holds them and `binSprites` grows them.
- [ ] `VulkanRenderer::sceneAt` const overload is a `const_cast` of the non-const one.
- [ ] `components/rtxvulkan/shaders/lib/bindings.glsl` declares set-0 bindings out of order
      (15 before 14) with set-2 bindings `0, 6, 5, 1, 2, 4, 3, 8, 9, 7, 10` interleaved among them.
      `tone.comp` declares binding 3 before binding 2. `visibilitypass.cpp` `sBindings` builds the
      layout by index, so the reader has no one list to compare.
- [ ] `components/rtxvulkan/shaders/lib/lights.glsl:237-254` places `litCosine`'s doc inside
      `weighLamps`'s doc, and `weighLamps`'s `@param` block above `considerLamp`.
- [ ] `apps/rtxtool/shot.hpp:19` includes `<components/rtx/renderer.hpp>` between two namespace
      blocks.
- [ ] `components/rtx/CMakeLists.txt` lists `compositequeue` after `spritetiles`.
      `components/rtxvulkan/CMakeLists.txt` lists `slottable.hpp` between `frameslots` and
      `gbuffer`. `apps/rtxtool/CMakeLists.txt` lists `lighting`, `motion`, `npc`, `objectstorage`,
      `posedactors`, `options`, `perfcontrol`, `picture`, `view`, `viewpoint`, `validationchoice`,
      `verify` out of alphabetical order.

## Harness command plumbing is copied per command

- [ ] `apps/rtxtool/main.cpp` `dispatch` (475-770) is one 300-line function of
      `variables["..."].as<...>()` copies, one block per command.
- [ ] `apps/rtxtool/bench.cpp` `runBench` (169-523) mixes timing, hashing, JSON, crossing
      accounting and reporting in one function. `report` and `writeJson` restate the same fields
      by hand in two formats.
- [ ] `apps/rtxtool/view.cpp` `runWindow` (116-511) holds a 100-line event lambda and the frame
      loop in one function.
- [ ] `apps/rtxtool/options.cpp:87-91` `--npc` help says "They come naked" while `--clothes`
      (line 101) defaults to on and dresses them.
- [ ] `files/rtx/views.cfg` `seyda-neen-customs` has a comment between two of its fields.

## Comments that describe code that no longer exists

- [ ] `apps/openmw/mwrender/rtx/rtxrenderer.hpp` `mComplained` carries the doc of another member
      ("The one sequence every mirror walk in this renderer poses at.").
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:437-443` `mNgx` has two merged doc paragraphs. The
      first says `describeDevice` takes a share of its own; `describeDevice` now calls
      `Dlss::probe`.
- [ ] `components/rtxvulkan/vulkanrenderer.hpp:407-419` the view-chain doc ("Its own chain and not
      the frame's ... Grown to the largest picture asked for") sits on `mViewScenes`.
- [ ] `components/rtxvulkan/structurestorage.hpp:40-42` says the renderer is synchronous and a
      fence-keyed retirement list has to grow here. `Graveyard` exists and `release` buries rooms
      in it.
- [ ] `components/rtxvulkan/commands.hpp:17-20` says `CommandPool` is "Setup only" and nothing in
      it is shaped for a frame. `allocate`, `begin` and `submit` are the frame path.
- [ ] `components/rtxvulkan/swapchain.cpp:29-30` says tone mapping "is M8's, and this is where they
      will land". `TonePass` exists.
- [ ] `components/rtxvulkan/gbuffer.cpp:67-69` says the mask is "One float ... See `gbuffer.h` for
      why it is not a byte". `gbuffer.h` defines `GBUFFER_MASK` as `R8_UNORM` and argues that it is
      a byte. `readChannel` widens bytes.
- [ ] `components/rtxvulkan/physicaldevice.cpp:66-68` "Why a candidate was rejected, or empty when
      it was not." sits above `hasResizableBar`.
- [ ] `components/rtxvulkan/texture.hpp:67` says "the shader is told the count and does not index
      past it". No count reaches the shader.
- [ ] `apps/rtxtool/cellscene.hpp:59-73` glues two doc blocks on `LoadedCell`, and 137-150 glues
      two on `RegionLoad`. `apps/rtxtool/stagedworld.hpp:112-119` puts `driveWeather`'s doc on
      `frame`.
- [ ] `apps/rtxtool/main.cpp:490-492` states "Boost skips the first token" twice.
- [ ] `components/rtxvulkan/shaders/lib/sprites.glsl:17-27` splits `puffLight`'s doc with an empty
      `///` line. `bindings.glsl:258-259` has a double blank line.
